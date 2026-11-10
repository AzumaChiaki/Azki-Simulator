/* SPDX-License-Identifier: GPL-2.0-or-later
 * Windows host I/O for private, disposable Azki instance files.
 * No firmware or guest-memory semantics are implemented here.
 */
#ifndef AZKI_WINDOWS_FILE_H
#define AZKI_WINDOWS_FILE_H

#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define AZKI_WIN_PATH_DEPTH 256u

typedef struct AzkiWinPath {
    wchar_t *name;
    HANDLE parents[AZKI_WIN_PATH_DEPTH];
    unsigned count;
} AzkiWinPath;

typedef struct AzkiWinSecurity {
    TOKEN_USER *user;
    PACL acl;
    SECURITY_DESCRIPTOR descriptor;
    SECURITY_ATTRIBUTES attributes;
} AzkiWinSecurity;

typedef struct AzkiWinMapping {
    HANDLE file;
    HANDLE mapping;
    void *view;
} AzkiWinMapping;

static inline int azki_win_error(DWORD error)
{
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND: errno = ENOENT; break;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD: errno = EACCES; break;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION: errno = EBUSY; break;
    case ERROR_INVALID_HANDLE: errno = EBADF; break;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY: errno = ENOMEM; break;
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_NAME:
    case ERROR_INVALID_DATA: errno = EINVAL; break;
    case ERROR_DISK_FULL: errno = ENOSPC; break;
    default: errno = EIO; break;
    }
    SetLastError(error);
    return -1;
}

static inline void azki_win_path_close(AzkiWinPath *path)
{
    while (path->count) {
        CloseHandle(path->parents[--path->count]);
    }
    free(path->name);
    path->name = NULL;
}

/* Only local absolute drive paths: no devices, ADS, UNC or DOS-name aliases. */
static inline bool azki_win_component(const wchar_t *name, size_t count)
{
    wchar_t stem[9] = { 0 };
    size_t length = 0;
    bool in_stem = true;
    if (!count || name[count - 1] == L'.' || name[count - 1] == L' ') {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        wchar_t c = name[i];
        if (c < 32 || wcschr(L"<>:\"|?*", c)) {
            return false;
        }
        if (c == L'.') {
            in_stem = false;
        }
        if (in_stem) {
            if (length < 8) {
                stem[length] = c >= L'a' && c <= L'z' ? c - L'a' + L'A' : c;
            }
            ++length;
        }
    }
    return !(length == 3 && (!wcscmp(stem, L"CON") || !wcscmp(stem, L"PRN") ||
                            !wcscmp(stem, L"AUX") || !wcscmp(stem, L"NUL"))) &&
           !(length == 4 && (!wcsncmp(stem, L"COM", 3) || !wcsncmp(stem, L"LPT", 3)) &&
             ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == 0xb9 ||
              stem[3] == 0xb2 || stem[3] == 0xb3)) &&
           wcscmp(stem, L"CONIN$") && wcscmp(stem, L"CONOUT$");
}

static inline bool azki_win_pin_directory(AzkiWinPath *path, size_t end)
{
    BY_HANDLE_FILE_INFORMATION info;
    wchar_t saved = path->name[end];
    HANDLE handle;
    if (path->count == AZKI_WIN_PATH_DEPTH) {
        azki_win_error(ERROR_INVALID_NAME);
        return false;
    }
    path->name[end] = 0;
    handle = CreateFileW(path->name, FILE_READ_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    path->name[end] = saved;
    if (handle == INVALID_HANDLE_VALUE) {
        azki_win_error(GetLastError());
        return false;
    }
    if (!GetFileInformationByHandle(handle, &info)) {
        DWORD error = GetLastError();
        CloseHandle(handle);
        azki_win_error(error);
        return false;
    }
    if (GetFileType(handle) != FILE_TYPE_DISK ||
        !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        CloseHandle(handle);
        azki_win_error(ERROR_ACCESS_DENIED);
        return false;
    }
    /* Omitting FILE_SHARE_DELETE pins each checked ancestor until final open. */
    path->parents[path->count++] = handle;
    return true;
}

static inline bool azki_win_path_open(AzkiWinPath *path, const char *utf8)
{
    int wide_count;
    wchar_t *input;
    DWORD needed, length;
    size_t begin;
    wchar_t drive[4];
    UINT kind;
    *path = (AzkiWinPath){ 0 };
    if (!utf8 || !*utf8 || strlen(utf8) > 131000) {
        azki_win_error(ERROR_INVALID_NAME);
        return false;
    }
    wide_count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
    if (wide_count < 4 || wide_count > 32760) {
        azki_win_error(ERROR_INVALID_NAME);
        return false;
    }
    input = malloc((size_t)wide_count * sizeof(*input));
    if (!input) {
        errno = ENOMEM;
        return false;
    }
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, input, wide_count)) {
        DWORD error = GetLastError();
        free(input);
        azki_win_error(error);
        return false;
    }
    for (int i = 0; i < wide_count; ++i) {
        if (input[i] == L'/') {
            input[i] = L'\\';
        }
    }
    if (!((input[0] >= L'A' && input[0] <= L'Z') ||
          (input[0] >= L'a' && input[0] <= L'z')) ||
        input[1] != L':' || input[2] != L'\\') {
        free(input);
        azki_win_error(ERROR_INVALID_NAME);
        return false;
    }
    drive[0] = input[0]; drive[1] = L':'; drive[2] = L'\\'; drive[3] = 0;
    kind = GetDriveTypeW(drive);
    if (kind != DRIVE_FIXED && kind != DRIVE_REMOVABLE) {
        free(input);
        azki_win_error(ERROR_ACCESS_DENIED);
        return false;
    }
    needed = GetFullPathNameW(input, 0, NULL, NULL);
    if (!needed || needed > 32760) {
        free(input);
        azki_win_error(ERROR_INVALID_NAME);
        return false;
    }
    path->name = calloc((size_t)needed + 4, sizeof(*path->name));
    if (!path->name) {
        free(input);
        errno = ENOMEM;
        return false;
    }
    memcpy(path->name, L"\\\\?\\", 4 * sizeof(wchar_t));
    length = GetFullPathNameW(input, needed, path->name + 4, NULL);
    free(input);
    if (!length || length >= needed || length <= 3) {
        azki_win_path_close(path);
        azki_win_error(ERROR_INVALID_NAME);
        return false;
    }
    length += 4;
    begin = 7;
    for (size_t i = 7; i <= length; ++i) {
        if (!path->name[i] || path->name[i] == L'\\') {
            if (!azki_win_component(path->name + begin, i - begin)) {
                azki_win_path_close(path);
                azki_win_error(ERROR_INVALID_NAME);
                return false;
            }
            begin = i + 1;
        }
    }
    if (!azki_win_pin_directory(path, 7)) {
        azki_win_path_close(path);
        return false;
    }
    for (size_t i = 7; i < length; ++i) {
        if (path->name[i] == L'\\' && !azki_win_pin_directory(path, i)) {
            azki_win_path_close(path);
            return false;
        }
    }
    return true;
}

static inline TOKEN_USER *azki_win_token_user(void)
{
    HANDLE token;
    DWORD bytes = 0, error;
    TOKEN_USER *user;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        azki_win_error(GetLastError());
        return NULL;
    }
    GetTokenInformation(token, TokenUser, NULL, 0, &bytes);
    if (!bytes) {
        error = GetLastError();
        CloseHandle(token);
        azki_win_error(error);
        return NULL;
    }
    user = malloc(bytes);
    if (!user) {
        CloseHandle(token);
        errno = ENOMEM;
        return NULL;
    }
    if (!GetTokenInformation(token, TokenUser, user, bytes, &bytes)) {
        error = GetLastError();
        CloseHandle(token);
        free(user);
        azki_win_error(error);
        return NULL;
    }
    CloseHandle(token);
    return user;
}

static inline void azki_win_security_close(AzkiWinSecurity *security)
{
    if (security->acl) {
        LocalFree(security->acl);
    }
    free(security->user);
    *security = (AzkiWinSecurity){ 0 };
}

static inline bool azki_win_security_open(AzkiWinSecurity *security)
{
    EXPLICIT_ACCESSW entry = { 0 };
    DWORD error;
    *security = (AzkiWinSecurity){ 0 };
    security->user = azki_win_token_user();
    if (!security->user) {
        return false;
    }
    entry.grfAccessPermissions = FILE_ALL_ACCESS;
    entry.grfAccessMode = SET_ACCESS;
    entry.grfInheritance = NO_INHERITANCE;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry.Trustee.TrusteeType = TRUSTEE_IS_USER;
    entry.Trustee.ptstrName = (LPWSTR)security->user->User.Sid;
    error = SetEntriesInAclW(1, &entry, NULL, &security->acl);
    if (error != ERROR_SUCCESS) {
        azki_win_security_close(security);
        azki_win_error(error);
        return false;
    }
    if (!InitializeSecurityDescriptor(&security->descriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorOwner(&security->descriptor, security->user->User.Sid, FALSE) ||
        !SetSecurityDescriptorDacl(&security->descriptor, TRUE, security->acl, FALSE) ||
        !SetSecurityDescriptorControl(&security->descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
        error = GetLastError();
        azki_win_security_close(security);
        azki_win_error(error);
        return false;
    }
    security->attributes.nLength = sizeof(security->attributes);
    security->attributes.lpSecurityDescriptor = &security->descriptor;
    security->attributes.bInheritHandle = FALSE;
    return true;
}

/* Current user must own the file. No unrelated account may receive an allow ACE.
 * SYSTEM/Administrators are accepted as Windows' privileged host principals. */
static inline bool azki_win_private_acl(HANDLE handle, PSID user)
{
    PSECURITY_DESCRIPTOR descriptor = NULL;
    PSID owner = NULL;
    PACL acl = NULL;
    BYTE system[SECURITY_MAX_SID_SIZE], administrators[SECURITY_MAX_SID_SIZE];
    DWORD system_bytes = sizeof(system), admin_bytes = sizeof(administrators);
    DWORD error = GetSecurityInfo(handle, SE_FILE_OBJECT,
                                  OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                  &owner, NULL, &acl, NULL, &descriptor);
    bool valid = false;
    if (error != ERROR_SUCCESS) {
        azki_win_error(error);
        return false;
    }
    if (!owner || !IsValidSid(owner) || !EqualSid(owner, user) || !acl ||
        !IsValidAcl(acl) || !CreateWellKnownSid(WinLocalSystemSid, NULL, system, &system_bytes) ||
        !CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, administrators, &admin_bytes)) {
        goto done;
    }
    for (DWORD i = 0; i < acl->AceCount; ++i) {
        ACE_HEADER *header;
        ACCESS_ALLOWED_ACE *ace;
        PSID sid;
        if (!GetAce(acl, i, (void **)&header)) {
            goto done;
        }
        if (header->AceFlags & INHERIT_ONLY_ACE) {
            continue;
        }
        if (header->AceType == ACCESS_DENIED_ACE_TYPE) {
            continue;
        }
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE ||
            header->AceSize < offsetof(ACCESS_ALLOWED_ACE, SidStart) + 8) {
            goto done;
        }
        ace = (ACCESS_ALLOWED_ACE *)header;
        sid = &ace->SidStart;
        if (!IsValidSid(sid) ||
            offsetof(ACCESS_ALLOWED_ACE, SidStart) + GetLengthSid(sid) > header->AceSize ||
            (ace->Mask && !EqualSid(sid, user) && !EqualSid(sid, system) &&
             !EqualSid(sid, administrators))) {
            goto done;
        }
    }
    valid = true;
done:
    LocalFree(descriptor);
    if (!valid) {
        azki_win_error(ERROR_ACCESS_DENIED);
    }
    return valid;
}

static inline bool azki_win_validate_handle(HANDLE handle, PSID user,
                                           bool exact_size, uint64_t bytes)
{
    BY_HANDLE_FILE_INFORMATION info;
    if (handle == INVALID_HANDLE_VALUE) {
        azki_win_error(ERROR_INVALID_HANDLE);
        return false;
    }
    if (GetFileType(handle) != FILE_TYPE_DISK) {
        azki_win_error(ERROR_ACCESS_DENIED);
        return false;
    }
    if (!GetFileInformationByHandle(handle, &info)) {
        azki_win_error(GetLastError());
        return false;
    }
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        info.nNumberOfLinks != 1 ||
        (exact_size && (((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow) != bytes)) {
        azki_win_error(ERROR_ACCESS_DENIED);
        return false;
    }
    return azki_win_private_acl(handle, user);
}

static inline HANDLE azki_win_open_private(const char *name, bool create,
                                          DWORD share, bool exact_size,
                                          uint64_t bytes, bool overlapped)
{
    AzkiWinPath path;
    AzkiWinSecurity security;
    HANDLE handle;
    if (!azki_win_path_open(&path, name)) {
        return INVALID_HANDLE_VALUE;
    }
    if (!azki_win_security_open(&security)) {
        azki_win_path_close(&path);
        return INVALID_HANDLE_VALUE;
    }
    handle = CreateFileW(path.name, GENERIC_READ | GENERIC_WRITE | READ_CONTROL, share,
                         &security.attributes, create ? OPEN_ALWAYS : OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                         (overlapped ? FILE_FLAG_OVERLAPPED : 0), NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        azki_win_error(GetLastError());
    } else if (!azki_win_validate_handle(handle, security.user->User.Sid, exact_size, bytes)) {
        CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
    }
    azki_win_security_close(&security);
    azki_win_path_close(&path);
    return handle;
}

static inline bool azki_win_store_valid(int fd, uint64_t bytes)
{
    HANDLE handle = (HANDLE)_get_osfhandle(fd);
    TOKEN_USER *user = azki_win_token_user();
    bool valid;
    if (!user) {
        return false;
    }
    valid = azki_win_validate_handle(handle, user->User.Sid, true, bytes);
    free(user);
    return valid;
}

static inline int azki_win_store_open(const char *name, uint64_t bytes)
{
    HANDLE handle = azki_win_open_private(name, false, 0, true, bytes, true);
    OVERLAPPED lock = { 0 };
    DWORD transferred, error;
    int fd;
    if (handle == INVALID_HANDLE_VALUE) {
        return -1;
    }
    lock.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!lock.hEvent) {
        error = GetLastError();
        CloseHandle(handle);
        return azki_win_error(error);
    }
    if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                    0, MAXDWORD, MAXDWORD, &lock)) {
        error = GetLastError();
        if (error != ERROR_IO_PENDING || !GetOverlappedResult(handle, &lock, &transferred, TRUE)) {
            if (error == ERROR_IO_PENDING) { error = GetLastError(); }
            CloseHandle(lock.hEvent);
            CloseHandle(handle);
            return azki_win_error(error);
        }
    }
    CloseHandle(lock.hEvent);
    fd = _open_osfhandle((intptr_t)handle, _O_RDWR | _O_BINARY | _O_NOINHERIT);
    if (fd < 0) {
        CloseHandle(handle);
    }
    return fd;
}

static inline int64_t azki_win_io_at(int fd, void *buffer, size_t bytes,
                                    uint64_t offset, bool write)
{
    HANDLE handle = (HANDLE)_get_osfhandle(fd);
    OVERLAPPED operation = { 0 };
    DWORD count = bytes > INT_MAX ? INT_MAX : (DWORD)bytes;
    DWORD transferred = 0, error;
    BOOL completed;
    if (handle == INVALID_HANDLE_VALUE || offset > INT64_MAX || count > INT64_MAX - offset) {
        return azki_win_error(ERROR_INVALID_PARAMETER);
    }
    if (!count) { return 0; }
    operation.Offset = (DWORD)offset;
    operation.OffsetHigh = (DWORD)(offset >> 32);
    operation.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!operation.hEvent) {
        return azki_win_error(GetLastError());
    }
    completed = write ? WriteFile(handle, buffer, count, NULL, &operation) :
                        ReadFile(handle, buffer, count, NULL, &operation);
    error = completed ? ERROR_SUCCESS : GetLastError();
    if ((completed || error == ERROR_IO_PENDING) &&
        GetOverlappedResult(handle, &operation, &transferred, TRUE)) {
        CloseHandle(operation.hEvent);
        return transferred;
    }
    if (completed || error == ERROR_IO_PENDING) { error = GetLastError(); }
    CloseHandle(operation.hEvent);
    if (!write && error == ERROR_HANDLE_EOF) { return 0; }
    return azki_win_error(error);
}

static inline int64_t azki_win_pread(int fd, void *buffer, size_t bytes, uint64_t offset)
{
    return azki_win_io_at(fd, buffer, bytes, offset, false);
}

static inline int64_t azki_win_pwrite(int fd, const void *buffer, size_t bytes, uint64_t offset)
{
    return azki_win_io_at(fd, (void *)buffer, bytes, offset, true);
}

static inline int azki_win_fsync(int fd)
{
    HANDLE handle = (HANDLE)_get_osfhandle(fd);
    if (handle == INVALID_HANDLE_VALUE) {
        return azki_win_error(ERROR_INVALID_HANDLE);
    }
    if (!FlushFileBuffers(handle)) {
        return azki_win_error(GetLastError());
    }
    return 0;
}

static inline void azki_win_mapping_close(AzkiWinMapping *mapping)
{
    if (mapping->view) { UnmapViewOfFile(mapping->view); }
    if (mapping->mapping) { CloseHandle(mapping->mapping); }
    if (mapping->file && mapping->file != INVALID_HANDLE_VALUE) { CloseHandle(mapping->file); }
    *mapping = (AzkiWinMapping){ 0 };
}

/* Retain the writer handle so independent readers may open/map, but another
 * writer or delete/rename cannot replace the file during the session. */
static inline bool azki_win_mapping_open(AzkiWinMapping *mapping,
                                         const char *name, uint64_t bytes)
{
    AzkiWinMapping next = { 0 };
    LARGE_INTEGER length;
    DWORD error;
    if (!mapping || mapping->view || !bytes || bytes > SIZE_MAX || bytes > INT64_MAX) {
        azki_win_error(ERROR_INVALID_PARAMETER);
        return false;
    }
    next.file = azki_win_open_private(name, true, FILE_SHARE_READ, false, 0, false);
    if (next.file == INVALID_HANDLE_VALUE) {
        return false;
    }
    length.QuadPart = (LONGLONG)bytes;
    if (!SetFilePointerEx(next.file, length, NULL, FILE_BEGIN) || !SetEndOfFile(next.file)) {
        error = GetLastError();
        azki_win_mapping_close(&next);
        azki_win_error(error);
        return false;
    }
    next.mapping = CreateFileMappingW(next.file, NULL, PAGE_READWRITE,
                                      (DWORD)(bytes >> 32), (DWORD)bytes, NULL);
    if (next.mapping) {
        next.view = MapViewOfFile(next.mapping, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, (SIZE_T)bytes);
    }
    if (!next.view) {
        error = GetLastError();
        azki_win_mapping_close(&next);
        azki_win_error(error);
        return false;
    }
    *mapping = next;
    return true;
}

#endif /* _WIN32 */
#endif /* AZKI_WINDOWS_FILE_H */
