/* UTF-16 process arguments -> strict UTF-8 helper ABI, binary standard streams. */
#ifdef _WIN32
#ifdef main
#undef main
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
int azki_helper_main(int argc, char **argv);
static int binary_stream(FILE *stream)
{
    /* GUI subprocesses may have an unassociated standard stream (fd -2).
     * There are no bytes to translate in that case; valid streams stay binary. */
    int fd = _fileno(stream);
    if (fd < 0) { return 0; }
    intptr_t handle = _get_osfhandle(fd);
    if (handle == -1 || handle == -2) { return 0; }
    return _setmode(fd, _O_BINARY) == -1 ? -1 : 0;
}
int wmain(int argc, wchar_t **wide)
{
    char **argv = calloc((size_t)argc + 1, sizeof(*argv));
    if (!argv) { return 2; }
    int status = 2;
    for (int i = 0; i < argc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, NULL, 0, NULL, NULL);
        if (!n || !(argv[i] = malloc((size_t)n)) ||
            !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, argv[i], n, NULL, NULL)) { goto done; }
    }
    if (binary_stream(stdin) || binary_stream(stdout) || binary_stream(stderr)) { goto done; }
    status = azki_helper_main(argc, argv);
done:
    for (int i = 0; i < argc; ++i) { free(argv[i]); }
    free(argv); return status;
}
#endif
