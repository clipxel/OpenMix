#!/bin/zsh
# Prepara el entorno de compilación: JUCE 8 + CMake/Ninja locales (sin Homebrew).
set -e
cd "$(dirname "$0")/.."
[ -d external/JUCE ] || git clone --depth 1 --branch 8.0.16-security https://github.com/juce-framework/JUCE.git external/JUCE
# Time-stretching para Key Lock (MIT)
[ -d external/signalsmith-stretch ] || git clone --depth 1 --branch 1.4.0 https://github.com/Signalsmith-Audio/signalsmith-stretch.git external/signalsmith-stretch
[ -d external/signalsmith-linear ] || git clone --depth 1 --branch 0.6.4 https://github.com/Signalsmith-Audio/linear.git external/signalsmith-linear
[ -x .tools/venv/bin/cmake ] || { python3 -m venv .tools/venv && .tools/venv/bin/pip install -q cmake ninja; }
.tools/venv/bin/cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM="$PWD/.tools/venv/bin/ninja"
.tools/venv/bin/ninja -C build OpenMix
echo "Listo: build/OpenMix_artefacts/Release/OpenMix.app"
