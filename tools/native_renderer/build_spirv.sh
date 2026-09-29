#!/bin/sh
# Builds the SPIR-V shaders the native renderer loads, from your own copy of
# the game. Output: <out dir>/<HASH>_vs.spv / <HASH>_ps.spv (derived from the
# game: keep it in private/).
#
# Usage: sh tools/native_renderer/build_spirv.sh [out dir]
#   RAYMAN_GAME_DIR may point at an extracted disc outside the repository.
#   Needs: $RAYMAN_GAME_DIR (or private/game), private/data/image.bin (tools/diag/imagedump),
#   tools/forks/xenosrecomp built (XenosRecomp + its DXC binaries).
set -e
P=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$P/private/native/spirv_ubo}
GAME=${RAYMAN_GAME_DIR:-$P/private/game}
X=$P/tools/forks/xenosrecomp
DXC_DIR=$X/thirdparty/dxc-bin
case $(uname -m) in
    arm64) DXC_ARCH=arm64 ;;
    x86_64) DXC_ARCH=x64 ;;
    *) echo "unsupported macOS host architecture: $(uname -m)" >&2; exit 1 ;;
esac
export DYLD_LIBRARY_PATH=$DXC_DIR/lib/$DXC_ARCH
DXC=$DXC_DIR/bin/$DXC_ARCH/dxc-macos
WORK=$OUT/work
mkdir -p "$OUT" "$WORK"

for required in \
    "$P/tools/native_renderer/shaderprep" \
    "$P/private/data/image.bin" \
    "$GAME/bootsequence_X360.ipk" \
    "$X/build/XenosRecomp/XenosRecomp" \
    "$DXC"; do
    if [ ! -f "$required" ]; then
        echo "missing shader-build input: $required" >&2
        exit 1
    fi
done

"$P/tools/native_renderer/shaderprep" "$GAME/bootsequence_X360.ipk" "$P/private/data/image.bin" "$WORK"

ok=0; skipped=0
for bin in "$WORK"/*.bin; do
    name=$(basename "$bin" .bin)
    if ! "$X/build/XenosRecomp/XenosRecomp" "$bin" "$WORK/$name.hlsl" "$X/XenosRecomp/shader_common.h" >/dev/null 2>&1; then
        skipped=$((skipped + 1)); continue
    fi
    python3 "$P/tools/native_renderer/hlsl_ubo.py" "$WORK/$name.hlsl" "$WORK/$name.ubo.hlsl" || { skipped=$((skipped + 1)); continue; }
    case $name in
        *_vs) set -- -T vs_6_0 -fvk-invert-y ;;
        *) set -- -T ps_6_0 ;;
    esac
    if "$DXC" "$@" -HV 2021 -all-resources-bound -spirv -fvk-use-dx-layout -Qstrip_debug -E main \
            -Fo "$OUT/$name.spv" "$WORK/$name.ubo.hlsl" > "$WORK/$name.log" 2>&1; then
        ok=$((ok + 1))
    else
        skipped=$((skipped + 1))
    fi
done
echo "SPIR-V: $ok shaders built, $skipped containers skipped (not convertible) -> $OUT"
