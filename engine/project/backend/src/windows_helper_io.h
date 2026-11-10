/* Win32 I/O for owned helper transactions; POSIX callers keep their APIs. */
#ifndef AZKI_WINDOWS_HELPER_IO_H
#define AZKI_WINDOWS_HELPER_IO_H
#ifdef _WIN32
#include "../../../qemu-overlay/hw/arm/azki-windows-file.h"
#include <stdio.h>
#include <wincrypt.h>

static inline char *azki_helper_utf8(const wchar_t *value)
{
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, NULL, 0, NULL, NULL);
    if (!n) { azki_win_error(GetLastError()); return NULL; }
    char *out = malloc((size_t)n);
    if (!out) { errno = ENOMEM; return NULL; }
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, out, n, NULL, NULL)) {
        DWORD error = GetLastError(); free(out); azki_win_error(error); return NULL;
    }
    return out;
}

/* CREATE_NEW is distinct from OPEN_EXISTING; neither silently truncates. */
static inline int azki_helper_open(const char *name, bool writing, bool create, bool offset_io)
{
    AzkiWinPath path;
    AzkiWinSecurity security;
    HANDLE handle;
    if (!azki_win_path_open(&path, name)) { return -1; }
    if (!azki_win_security_open(&security)) { azki_win_path_close(&path); return -1; }
    handle = CreateFileW(path.name, GENERIC_READ | (writing ? GENERIC_WRITE : 0) | READ_CONTROL,
                         0, &security.attributes, create ? CREATE_NEW : OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                         (offset_io ? FILE_FLAG_OVERLAPPED : 0), NULL);
    if (handle == INVALID_HANDLE_VALUE) { azki_win_error(GetLastError()); }
    else if (!azki_win_validate_handle(handle, security.user->User.Sid, false, 0)) {
        CloseHandle(handle); handle = INVALID_HANDLE_VALUE;
    }
    azki_win_security_close(&security); azki_win_path_close(&path);
    if (handle == INVALID_HANDLE_VALUE) { return -1; }
    if (offset_io) {
        OVERLAPPED lock = {0}; DWORD transferred, error;
        lock.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!lock.hEvent) { error = GetLastError(); CloseHandle(handle); return azki_win_error(error); }
        BOOL ok = LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                             0, MAXDWORD, MAXDWORD, &lock);
        error = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && (error != ERROR_IO_PENDING || !GetOverlappedResult(handle, &lock, &transferred, TRUE))) {
            if (error == ERROR_IO_PENDING) { error = GetLastError(); }
            CloseHandle(lock.hEvent); CloseHandle(handle); return azki_win_error(error);
        }
        CloseHandle(lock.hEvent);
    }
    int fd = _open_osfhandle((intptr_t)handle, (writing ? _O_RDWR : _O_RDONLY) | _O_BINARY | _O_NOINHERIT);
    if (fd < 0) { CloseHandle(handle); }
    return fd;
}

static inline FILE *azki_helper_stream(const char *path, bool create)
{
    int fd = azki_helper_open(path, create, create, false);
    if (fd < 0) { return NULL; }
    FILE *file = _fdopen(fd, create ? "wb" : "rb");
    if (!file) { _close(fd); }
    return file;
}

static inline HANDLE azki_helper_directory(const char *name)
{
    AzkiWinPath path; AzkiWinSecurity security;
    if (!azki_win_path_open(&path, name)) { return INVALID_HANDLE_VALUE; }
    if (!azki_win_security_open(&security)) { azki_win_path_close(&path); return INVALID_HANDLE_VALUE; }
    HANDLE handle = CreateFileW(path.name, GENERIC_READ | READ_CONTROL,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    BY_HANDLE_FILE_INFORMATION info;
    if (handle == INVALID_HANDLE_VALUE) { azki_win_error(GetLastError()); }
    else if (!GetFileInformationByHandle(handle, &info) || GetFileType(handle) != FILE_TYPE_DISK ||
             !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
             (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
             !azki_win_private_acl(handle, security.user->User.Sid)) {
        CloseHandle(handle); handle = INVALID_HANDLE_VALUE; errno = EACCES;
    }
    azki_win_security_close(&security); azki_win_path_close(&path);
    return handle;
}

/* Own directory with inheritable user-only permissions, including files later
 * created by UnQLite's Win32 VFS. Never inherit the host temp directory's ACL. */
static inline bool azki_helper_mkdir(const char *name)
{
    AzkiWinPath path; AzkiWinSecurity security; PACL inherited = NULL;
    if (!azki_win_path_open(&path, name)) { return false; }
    if (!azki_win_security_open(&security)) { azki_win_path_close(&path); return false; }
    EXPLICIT_ACCESSW entry = {0};
    entry.grfAccessPermissions = FILE_ALL_ACCESS; entry.grfAccessMode = SET_ACCESS;
    entry.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID; entry.Trustee.TrusteeType = TRUSTEE_IS_USER;
    entry.Trustee.ptstrName = (LPWSTR)security.user->User.Sid;
    DWORD error = SetEntriesInAclW(1, &entry, NULL, &inherited);
    BOOL ok = error == ERROR_SUCCESS && SetSecurityDescriptorDacl(&security.descriptor, TRUE, inherited, FALSE);
    if (ok) { ok = CreateDirectoryW(path.name, &security.attributes); error = ok ? ERROR_SUCCESS : GetLastError(); }
    else if (error == ERROR_SUCCESS) { error = GetLastError(); }
    if (inherited) { LocalFree(inherited); }
    azki_win_security_close(&security); azki_win_path_close(&path);
    if (!ok) { azki_win_error(error); }
    return ok != FALSE;
}

static inline bool azki_helper_tempdir(char *out, size_t capacity)
{
    const wchar_t *parent = _wgetenv(L"TMPDIR");
    if (!parent || !*parent) { parent = _wgetenv(L"TEMP"); }
    if (!parent || !*parent) { errno = ENOENT; return false; }
    char *utf8 = azki_helper_utf8(parent);
    if (!utf8) { return false; }
    HCRYPTPROV provider = 0;
    if (!CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
        DWORD error = GetLastError(); free(utf8); azki_win_error(error); return false;
    }
    bool success = false;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        unsigned char random[16]; char suffix[33];
        if (!CryptGenRandom(provider, sizeof(random), random)) { azki_win_error(GetLastError()); break; }
        for (unsigned i = 0; i < 16; ++i) { snprintf(suffix + i * 2, 3, "%02x", random[i]); }
        int count = snprintf(out, capacity, "%s/instance-props-%s", utf8, suffix);
        if (count < 0 || (size_t)count >= capacity) { errno = ENAMETOOLONG; break; }
        if (azki_helper_mkdir(out)) { success = true; break; }
        if (GetLastError() != ERROR_ALREADY_EXISTS) { break; }
    }
    CryptReleaseContext(provider, 0); free(utf8); return success;
}

static inline int azki_helper_remove(const char *name, bool directory)
{
    AzkiWinPath path;
    if (!azki_win_path_open(&path, name)) { return -1; }
    BOOL ok = directory ? RemoveDirectoryW(path.name) : DeleteFileW(path.name);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    azki_win_path_close(&path); return ok ? 0 : azki_win_error(error);
}
#endif
#endif
