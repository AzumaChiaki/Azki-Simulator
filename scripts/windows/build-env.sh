#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C.UTF-8 LANG=C.UTF-8
exec /ucrt64/bin/python "$(cygpath -u "$AZKI_WIN_BUILD_DRIVER")" \
    --source-archive "$AZKI_WIN_SOURCE_ARCHIVE" \
    --work-root "$AZKI_WIN_BUILD_CACHE" \
    --runtime-python "$AZKI_WIN_RUNTIME_PYTHON" \
    --jobs "$AZKI_WIN_BUILD_JOBS"
