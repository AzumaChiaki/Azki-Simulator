#!/bin/bash
# Full boot acceptance for a built Linux package.
#
# The work is done by the integration suite, so the ladder cannot drift from
# the tests: this script only establishes the contract around it -- the
# extracted package, the authorized firmware, and a sandbox that can actually
# start. Every refusal here is a precondition, not a verdict.
#
# Usage: AZKI_FIRMWARE=/path/to/ota.zip scripts/verify-linux-boot.sh /path/to/azki-simulator
set -euo pipefail
root="$(cd -- "$(dirname -- "$0")/.." && pwd)"
package="${1:-}"
shift || true
firmware="${AZKI_FIRMWARE:-}"

if [ -z "$package" ] || [ -z "$firmware" ]; then
  echo "Usage: AZKI_FIRMWARE=/path/to/ota.zip $0 /path/to/extracted/azki-simulator" >&2
  exit 2
fi
test -d "$package" || { echo "打包件目录不存在：$package" >&2; exit 2; }
test -f "$firmware" || { echo "固件不存在：$firmware（仓库内不含固件）" >&2; exit 2; }
for required in Engine/python/bin/python3.12 Engine/manifest.json App/AzkiSimulator.Linux.dll; do
  test -e "$package/$required" || { echo "打包件缺少 $required" >&2; exit 2; }
done
command -v bwrap >/dev/null || {
  echo 'PATH 里没有 bwrap：Linux 会话靠它做隔离，缺它就是硬失败，不回落裸跑。' >&2
  exit 2
}
# The sandbox needs user namespaces. A hardened kernel turns them off, and the
# failure inside the test would otherwise read like a firmware problem.
if ! bwrap --ro-bind / / --dev /dev --unshare-all true >/dev/null 2>&1; then
  echo 'bwrap 起不来：多半是内核禁用了 unprivileged user namespace（查 kernel.unprivileged_userns_clone）。' >&2
  exit 2
fi

emulators_before="$(pgrep -x qemu-system-arm 2>/dev/null | sort || true)"
export AZKI_APP_ROOT="$package"
export DOTNET_CLI_TELEMETRY_OPTOUT=1
export DOTNET_NOLOGO=1
status=0
dotnet test "$root/linux/AzkiSimulator.Linux.sln" --filter "Category=Integration" "$@" || status=$?

emulators_after="$(pgrep -x qemu-system-arm 2>/dev/null | sort || true)"
leaked="$(comm -13 <(echo "$emulators_before") <(echo "$emulators_after") || true)"
if [ -n "$leaked" ]; then
  echo '验收结束后仍有本次启动的模拟器残留：' >&2
  echo "$leaked" >&2
  status=1
fi
exit "$status"
