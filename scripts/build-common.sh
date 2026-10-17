#!/bin/bash
# Shared policy: only final artifacts in dist; all work is disposable and external.
azki_dist_output() {
  python3 -B - "$root" "${1:-$root/dist}" <<'PY'
import pathlib, sys
root = pathlib.Path(sys.argv[1]).resolve()
expected = root / 'dist'
candidate = pathlib.Path(sys.argv[2]).resolve()
if candidate != expected or expected.is_symlink():
    raise SystemExit('Final build output must be the repository dist directory; custom output directories are not supported.')
expected.mkdir(exist_ok=True)
print(expected)
PY
}

azki_temporary_work() {
  if [ -n "${AZKI_BUILD_DIR:-}" ] || [ -n "${AZKI_WORK_DIR:-}" ]; then
    echo 'AZKI_BUILD_DIR/AZKI_WORK_DIR are no longer supported; build work is temporary and cleaned automatically.' >&2
    return 2
  fi
  azki_work="$(mktemp -d /private/tmp/azki-build.XXXXXXXX)"
  trap azki_cleanup_work EXIT
  trap 'exit 129' HUP
  trap 'exit 130' INT
  trap 'exit 143' TERM
}

azki_cleanup_work() {
  case "${azki_work:-}" in
    /private/tmp/azki-build.*) rm -rf -- "$azki_work" ;;
  esac
}
