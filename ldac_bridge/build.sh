#!/bin/sh
# Build ldac_bridge.exe with MSYS2 UCRT64 gcc.
set -e
cd "$(dirname "$0")"
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
BTSTACK=$(cygpath -m -s ../third_party/btstack)
LIBLDAC=$(cygpath -m -s ../third_party/libldac)
mingw32-make -j8 BTSTACK="$BTSTACK" LIBLDAC="$LIBLDAC" "$@"
