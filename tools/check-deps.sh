#!/bin/bash
# Checks everything needed to build this project before any compiler runs.
# Exits non-zero with a clear reason on the first thing missing.
#
#   tools/check-deps.sh            # check driver + tools + python deps (what quickstart.sh needs)
#   tools/check-deps.sh --firmware # also check the PlatformIO toolchain
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
fail=0

need() {
    # need <binary> <apt/pacman-ish hint>
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "missing: $1 ($2)" >&2
        fail=1
    fi
}

echo "== checking build dependencies (host: $(uname -m), kernel $(uname -r)) =="

need make "build-essential / base-devel"
need cc "build-essential / base-devel (gcc or clang)"

kdir="/lib/modules/$(uname -r)/build"
if [ ! -d "$kdir" ]; then
    echo "missing: kernel headers at $kdir (install the headers/devel package matching THIS running kernel/arch)" >&2
    fail=1
fi

need python3 "python3"
if ! python3 -c "import serial" >/dev/null 2>&1; then
    echo "missing: python3 'serial' module (pip install pyserial) - needed by tools/probe.py and tools/fake-esp.py" >&2
    fail=1
fi

if [ "$1" = "--firmware" ]; then
    need pio "pip install platformio"
fi

# Needed to activate the link once the module is up, not to build it -
# warn rather than fail so a build-only dependency check still succeeds.
for b in iwctl rfkill; do
    command -v "$b" >/dev/null 2>&1 || echo "note: $b not found - needed later to bring the link up, not to build" >&2
done

if [ "$fail" -ne 0 ]; then
    echo "== missing required dependencies, see above ==" >&2
    exit 1
fi
echo "== all required dependencies present =="
