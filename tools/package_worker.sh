#!/usr/bin/env bash
# Step 4.19 — build a one-command join package.
#
# WHY THIS EXISTS (RISKS.md R-D). The non-Apple GPU need is bursty: Phase 4
# wants it hands-on, Phases 5-6 do not, and Phase 7 hard-requires it again.
# Every visit that needs a toolchain install, a vcpkg bootstrap and a 20-minute
# build is a visit that does not happen. This turns the ask into "run one
# command" — which is the difference between borrowing a machine for an evening
# and borrowing it for a weekend.
#
# Deliberately NOT a container: the whole point is reaching a real GPU, and
# getting a GPU into a container is the setup burden this exists to remove.
#
#   ./tools/package_worker.sh [build-dir] [out-dir]
#
# Produces p2pgpu-worker-<os>-<arch>.tar.gz containing the binary, the kernels
# it reads at runtime, and a `join` script. CROSS-COMPILING IS NOT ATTEMPTED —
# a package is built on, and for, one platform. CI builds the Linux one; a
# Windows package needs a Windows runner (see the note at the end).

set -euo pipefail

BUILD_DIR="${1:-build/native-release}"
OUT_DIR="${2:-dist}"

OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
ARCH="$(uname -m)"
NAME="p2pgpu-worker-${OS}-${ARCH}"
STAGE="${OUT_DIR}/${NAME}"

if [[ ! -x "${BUILD_DIR}/worker-native" ]]; then
  echo "error: ${BUILD_DIR}/worker-native not found." >&2
  echo "       cmake --preset native-release && cmake --build ${BUILD_DIR}" >&2
  exit 1
fi

rm -rf "${STAGE}"
mkdir -p "${STAGE}/kernels"

cp "${BUILD_DIR}/worker-native" "${STAGE}/"
# The kernels are READ AT RUNTIME, not baked in — the coordinator serves WGSL
# to browser workers over HTTP, and the native worker reads the same files from
# disk. Shipping the binary alone produces a worker that starts, connects, and
# then fails every task with KernelUnavailable.
cp kernels/*.wgsl kernels/manifest.toml "${STAGE}/kernels/"

cat > "${STAGE}/join" <<'LAUNCH'
#!/usr/bin/env bash
# Join a p2pgpu fleet. One argument: the coordinator's WebSocket URL.
#
#   ./join ws://example.com:8080/ws
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ $# -lt 1 ]]; then
  echo "usage: ./join ws://HOST:PORT/ws" >&2
  exit 2
fi

# --kernel-dir is passed explicitly rather than relying on the working
# directory: the owner will run this from wherever they unpacked it, and a
# relative default would fail in a way that looks like a network problem.
exec "${HERE}/worker-native" run \
  --coordinator "$1" \
  --kernel-dir "${HERE}/kernels" \
  "${@:2}"
LAUNCH
chmod +x "${STAGE}/join"

cat > "${STAGE}/README.txt" <<'DOC'
p2pgpu volunteer worker
=======================

WHAT THIS DOES
  Runs GPU compute for a research project on distributed computing. It
  contributes spare GPU time to a job and sends back results.

WHAT IT DOES NOT DO
  It reads no files of yours, opens no ports, and installs nothing. It makes
  ONE outbound WebSocket connection to the coordinator URL you pass it, and
  stops the moment you press Ctrl-C.

TO RUN
  ./join ws://HOST:PORT/ws

TO STOP
  Ctrl-C. Any work in progress is handed back to the coordinator
  automatically — nothing is lost by quitting at any moment.

TO CHECK WHAT GPU IT SEES, WITHOUT CONTRIBUTING ANYTHING
  ./worker-native adapter

USEFUL EXTRAS
  ./join ws://HOST:PORT/ws --throttle 0.5     use ~half the GPU
DOC

tar -czf "${OUT_DIR}/${NAME}.tar.gz" -C "${OUT_DIR}" "${NAME}"
rm -rf "${STAGE}"

echo "built ${OUT_DIR}/${NAME}.tar.gz"
tar -tzf "${OUT_DIR}/${NAME}.tar.gz" | sed 's/^/  /'

# A Windows package is NOT produced here and cannot be: this script builds for
# the platform it runs on, and the borrowed machine is usually Windows. The
# browser worker is the Windows answer today (it needs no install at all), and
# a native Windows package needs a windows-latest CI runner — worth adding when
# a Windows-only measurement demands the native path.
