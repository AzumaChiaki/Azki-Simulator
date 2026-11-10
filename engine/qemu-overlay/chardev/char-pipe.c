/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/option.h"
#include "chardev/char.h"

#ifdef _WIN32
#include "chardev/char-win.h"
#include <sddl.h>
#else
#include "chardev/char-fd.h"
#endif

#ifdef _WIN32
#define MAXCONNECT 1
#define NTIMEOUT 5000

/* Explicit current-user/SYSTEM ACL, with optional runtime PID binding.
 * Invalid configured PID values are errors, never a disabled guard. */
static bool azki_pipe_security(SECURITY_ATTRIBUTES *attributes,
                              PSECURITY_DESCRIPTOR *descriptor,
                              DWORD *client_pid, Error **errp)
{
    const char *configured = getenv("AZKI_QEMU_PIPE_CLIENT_PID");
    HANDLE token = NULL;
    TOKEN_USER *user = NULL;
    LPSTR sid = NULL;
    char *sddl = NULL;
    DWORD bytes = 0;
    bool valid = false;
    *client_pid = 0;
    *descriptor = NULL;
    if (configured) {
        uint64_t number = 0;
        if (!*configured) {
            error_setg(errp, "AZKI_QEMU_PIPE_CLIENT_PID must be a positive decimal PID");
            return false;
        }
        for (const char *at = configured; *at; ++at) {
            if (*at < '0' || *at > '9' ||
                (number = number * 10 + (*at - '0')) > MAXDWORD) {
                error_setg(errp, "AZKI_QEMU_PIPE_CLIENT_PID must be a bounded decimal PID");
                return false;
            }
        }
        if (!number || number == GetCurrentProcessId()) {
            error_setg(errp, "AZKI_QEMU_PIPE_CLIENT_PID must identify the external runtime");
            return false;
        }
        *client_pid = (DWORD)number;
    }
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        error_setg_win32(errp, GetLastError(), "Open pipe-owner token");
        goto done;
    }
    GetTokenInformation(token, TokenUser, NULL, 0, &bytes);
    if (!bytes || bytes > 65536) {
        error_setg(errp, "Invalid pipe-owner token size");
        goto done;
    }
    user = g_malloc(bytes);
    if (!GetTokenInformation(token, TokenUser, user, bytes, &bytes) ||
        !ConvertSidToStringSidA(user->User.Sid, &sid)) {
        error_setg_win32(errp, GetLastError(), "Resolve private pipe owner");
        goto done;
    }
    sddl = g_strdup_printf("O:%sD:P(A;;GA;;;%s)(A;;GA;;;SY)", sid, sid);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
            sddl, SDDL_REVISION_1, descriptor, NULL)) {
        error_setg_win32(errp, GetLastError(), "Build private pipe ACL");
        goto done;
    }
    *attributes = (SECURITY_ATTRIBUTES) {
        .nLength = sizeof(*attributes),
        .lpSecurityDescriptor = *descriptor,
        .bInheritHandle = FALSE,
    };
    valid = true;
done:
    if (token) { CloseHandle(token); }
    if (sid) { LocalFree(sid); }
    g_free(user);
    g_free(sddl);
    return valid;
}

static int win_chr_pipe_init(Chardev *chr, const char *filename,
                             Error **errp)
{
    WinChardev *s = WIN_CHARDEV(chr);
    OVERLAPPED ov = { 0 };
    SECURITY_ATTRIBUTES attributes;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    DWORD expected_client, actual_client, size;
    int ret;
    char *openname;

    s->fpipe = TRUE;
    if (!azki_pipe_security(&attributes, &descriptor, &expected_client, errp)) {
        return -1;
    }
    s->hsend = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!s->hsend) {
        error_setg_win32(errp, GetLastError(), "Create private pipe send event");
        goto fail;
    }
    s->hrecv = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!s->hrecv) {
        error_setg_win32(errp, GetLastError(), "Create private pipe receive event");
        goto fail;
    }
    openname = g_strdup_printf("\\\\.\\pipe\\%s", filename);
    s->file = CreateNamedPipe(openname,
                              PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                              FILE_FLAG_FIRST_PIPE_INSTANCE,
                              PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                              PIPE_REJECT_REMOTE_CLIENTS,
                              MAXCONNECT, NSENDBUF, NRECVBUF, NTIMEOUT, &attributes);
    LocalFree(descriptor);
    descriptor = NULL;
    g_free(openname);
    if (s->file == INVALID_HANDLE_VALUE) {
        error_setg_win32(errp, GetLastError(), "Create private named pipe");
        s->file = NULL;
        goto fail;
    }
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) {
        error_setg_win32(errp, GetLastError(), "Create pipe-connect event");
        goto fail;
    }
    ret = ConnectNamedPipe(s->file, &ov);
    if (!ret) {
        DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            if (!GetOverlappedResult(s->file, &ov, &size, TRUE)) {
                error_setg_win32(errp, GetLastError(), "Wait for private pipe client");
                goto fail;
            }
        } else if (error != ERROR_PIPE_CONNECTED) {
            error_setg_win32(errp, error, "Connect private pipe client");
            goto fail;
        }
    }
    if (expected_client) {
        if (!GetNamedPipeClientProcessId(s->file, &actual_client)) {
            error_setg_win32(errp, GetLastError(), "Identify private pipe client");
            goto fail;
        }
        if (actual_client != expected_client) {
            error_setg(errp, "Private pipe client PID differs from owned runtime");
            goto fail;
        }
    }
    CloseHandle(ov.hEvent);
    ov.hEvent = NULL;
    qemu_add_polling_cb(win_chr_pipe_poll, chr);
    return 0;

 fail:
    if (descriptor) { LocalFree(descriptor); }
    if (ov.hEvent) { CloseHandle(ov.hEvent); }
    if (s->file && s->file != INVALID_HANDLE_VALUE) { CloseHandle(s->file); }
    if (s->hsend) { CloseHandle(s->hsend); }
    if (s->hrecv) { CloseHandle(s->hrecv); }
    s->file = s->hsend = s->hrecv = NULL;
    return -1;
}

static void qemu_chr_open_pipe(Chardev *chr,
                               ChardevBackend *backend,
                               bool *be_opened,
                               Error **errp)
{
    ChardevHostdev *opts = backend->u.pipe.data;
    const char *filename = opts->device;

    if (win_chr_pipe_init(chr, filename, errp) < 0) {
        return;
    }
}

#else

static void qemu_chr_open_pipe(Chardev *chr,
                               ChardevBackend *backend,
                               bool *be_opened,
                               Error **errp)
{
    ChardevHostdev *opts = backend->u.pipe.data;
    int fd_in, fd_out;
    char *filename_in;
    char *filename_out;
    const char *filename = opts->device;

    filename_in = g_strdup_printf("%s.in", filename);
    filename_out = g_strdup_printf("%s.out", filename);
    fd_in = RETRY_ON_EINTR(qemu_open_old(filename_in, O_RDWR | O_BINARY));
    fd_out = RETRY_ON_EINTR(qemu_open_old(filename_out, O_RDWR | O_BINARY));
    g_free(filename_in);
    g_free(filename_out);
    if (fd_in < 0 || fd_out < 0) {
        if (fd_in >= 0) {
            close(fd_in);
        }
        if (fd_out >= 0) {
            close(fd_out);
        }
        fd_in = fd_out = RETRY_ON_EINTR(
            qemu_open_old(filename, O_RDWR | O_BINARY)
        );
        if (fd_in < 0) {
            error_setg_file_open(errp, errno, filename);
            return;
        }
    }
    qemu_chr_open_fd(chr, fd_in, fd_out);
}

#endif /* !_WIN32 */

static void qemu_chr_parse_pipe(QemuOpts *opts, ChardevBackend *backend,
                                Error **errp)
{
    const char *device = qemu_opt_get(opts, "path");
    ChardevHostdev *dev;

    if (device == NULL) {
        error_setg(errp, "chardev: pipe: no device path given");
        return;
    }
    backend->type = CHARDEV_BACKEND_KIND_PIPE;
    dev = backend->u.pipe.data = g_new0(ChardevHostdev, 1);
    qemu_chr_parse_common(opts, qapi_ChardevHostdev_base(dev));
    dev->device = g_strdup(device);
}

static void char_pipe_class_init(ObjectClass *oc, const void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->parse = qemu_chr_parse_pipe;
    cc->open = qemu_chr_open_pipe;
}

static const TypeInfo char_pipe_type_info = {
    .name = TYPE_CHARDEV_PIPE,
#ifdef _WIN32
    .parent = TYPE_CHARDEV_WIN,
#else
    .parent = TYPE_CHARDEV_FD,
#endif
    .class_init = char_pipe_class_init,
};

static void register_types(void)
{
    type_register_static(&char_pipe_type_info);
}

type_init(register_types);
