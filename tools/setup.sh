#!/bin/sh
# Prepara as ferramentas: submódulo do XenonRecomp, build das ferramentas e,
# no macOS, o LLVM oficial (o Homebrew compila o LLVM do zero no macOS 14).
set -e
P=$(cd "$(dirname "$0")/.." && pwd)
cd "$P"

git submodule update --init --recursive

if [ "$(uname)" = "Darwin" ] && [ "$(uname -m)" = "arm64" ] && [ ! -x tools/llvm/LLVM-23.1.2-macOS-ARM64/bin/clang++ ]; then
    echo "Baixando LLVM 23.1.2 (macOS ARM64, ~1.5 GB)..."
    mkdir -p tools/llvm
    curl -L --fail -o tools/llvm/llvm.tar.xz \
        https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.2/LLVM-23.1.2-macOS-ARM64.tar.xz
    tar -xJf tools/llvm/llvm.tar.xz -C tools/llvm
    rm tools/llvm/llvm.tar.xz
fi

# Intel Macs do not have a matching bundled LLVM archive. Use the host Clang
# instead (Apple Clang 18+ is sufficient for the host tools).
if [ "$(uname)" = "Darwin" ] && [ "$(uname -m)" = "x86_64" ]; then
    echo "macOS Intel: using host Clang"
fi

if [ "$(uname)" = "Darwin" ] && [ ! -f tools/moltenvk/MoltenVK/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib ]; then
    echo "Baixando MoltenVK 1.4.2 (Vulkan sobre Metal)..."
    mkdir -p tools/moltenvk
    curl -L --fail -o tools/moltenvk/mvk.tar \
        https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.2/MoltenVK-macos.tar
    tar -xf tools/moltenvk/mvk.tar -C tools/moltenvk
    rm tools/moltenvk/mvk.tar
fi

CC="${CC:-clang}" CXX="${CXX:-clang++}" cmake -S tools/XenonRecomp -B tools/XenonRecomp/build -G Ninja -DCMAKE_BUILD_TYPE=Release -Wno-dev
ninja -C tools/XenonRecomp/build XenonRecomp XenonAnalyse

sh tools/jumptables/build.sh
sh tools/diag/build.sh
echo "Pronto. Coloque seu default.xex em private/game/ e rode: sh tools/regen_config.sh"
