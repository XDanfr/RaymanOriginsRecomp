#!/bin/sh
# Validate the relocatable Switch NVK package expected by the Rayman CMake
# target. This only inspects public headers/libraries; it does not build or
# install the driver.
set -eu

ROOT=${1:-${RAYMAN_SWITCH_NVK_ROOT:-}}
DEVKIT=${DEVKITPRO:-/opt/devkitpro}
NM=$DEVKIT/devkitA64/bin/aarch64-none-elf-nm

if [ -z "$ROOT" ]; then
    echo "usage: sh tools/check_switch_nvk.sh /path/to/nvk-switch" >&2
    exit 2
fi

HEADER=$ROOT/include/vulkan/vulkan.h
VI_HEADER=$ROOT/include/vulkan/vulkan_vi.h
LIBRARY=$ROOT/lib/libvulkan.a
for required in "$HEADER" "$VI_HEADER" "$LIBRARY" "$NM"; do
    if [ ! -f "$required" ]; then
        echo "missing Switch NVK input: $required" >&2
        exit 1
    fi
done

for name in z zstd expat; do
    library=$DEVKIT/portlibs/switch/lib/lib$name.a
    if [ ! -f "$library" ]; then
        echo "missing Switch portlib required by NVK: $library" >&2
        exit 1
    fi
done

SYMBOLS=$(mktemp "${TMPDIR:-/tmp}/rayman-nvk-symbols.XXXXXX")
trap 'rm -f "$SYMBOLS"' EXIT HUP INT TERM
$NM "$LIBRARY" >"$SYMBOLS" 2>/dev/null
for symbol in \
    vk_icdGetInstanceProcAddr \
    __wrap_vk_icdGetInstanceProcAddr \
    __wrap_open \
    __wrap_close \
    __wrap_stat \
    __wrap_lstat; do
    if ! grep -Eq " [Tt] $symbol$" "$SYMBOLS"; then
        echo "Switch NVK archive does not define $symbol: $LIBRARY" >&2
        exit 1
    fi
done

if ! grep -Eiq 'CreateViSurfaceNN|wsi_switch_init_wsi' "$SYMBOLS"; then
    echo "Switch NVK archive has no VK_NN_vi_surface implementation: $LIBRARY" >&2
    exit 1
fi

echo "Switch NVK package OK"
echo "  root: $ROOT"
echo "  archive: $LIBRARY"
echo "  VI surface and loaderless ICD shims: present"
echo "  zlib, zstd and expat Switch portlibs: present"
