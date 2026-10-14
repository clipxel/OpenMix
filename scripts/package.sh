#!/bin/bash
# Empaqueta OpenMix para macOS: compila Release, corre los autotests, firma (ad-hoc) y arma
# dist/OpenMix-<versión>-macOS-universal.dmg con el acceso directo a Aplicaciones.
#
#   ./scripts/package.sh
#
# Para firmar con un Developer ID (y poder notarizar): SIGN_ID="Developer ID Application: …" ./scripts/package.sh
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION=$(sed -n 's/^project(OpenMix VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
APP=build/OpenMix_artefacts/Release/OpenMix.app
DMG=dist/OpenMix-$VERSION-macOS-universal.dmg     # Apple Silicon + Intel

.tools/venv/bin/cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
.tools/venv/bin/ninja -C build

# Autotests (señales sintéticas: no hace falta placa de audio)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
python3 - "$TMP/a.wav" "$TMP/b.wav" <<'EOF'
import math, struct, sys, wave
def make(path, bpm, base):
    sr, dur = 44100, 60
    frames = bytearray()
    for i in range(sr * dur):
        t = i / sr; ph = t % (60 / bpm)
        v = math.sin(2 * math.pi * (50 + 120 * math.exp(-ph * 30)) * ph) * math.exp(-ph * 8) * 0.8
        v += math.sin(2 * math.pi * base * t) * 0.2 * (1 - math.exp(-ph * 20))
        s = int(max(-1, min(1, v)) * 30000)
        frames += struct.pack('<hh', s, s)
    w = wave.open(path, 'wb'); w.setnchannels(2); w.setsampwidth(2); w.setframerate(sr); w.writeframes(bytes(frames)); w.close()
make(sys.argv[1], 124, 55); make(sys.argv[2], 128, 49)
EOF
"$APP/Contents/MacOS/OpenMix" --selftest --data "$TMP/data" --a "$TMP/a.wav" --b "$TMP/b.wav" > "$TMP/selftest.log" 2>&1 \
    || { grep FAIL "$TMP/selftest.log"; echo "Autotests fallidos: no se empaqueta."; exit 1; }
echo "Autotests: $(grep -c PASS "$TMP/selftest.log") OK"

if [ -n "${SIGN_ID:-}" ]; then
    codesign --force --deep --options runtime -s "$SIGN_ID" "$APP"   # notarizable (hardened runtime)
else
    codesign --force --deep -s - "$APP"                              # ad-hoc: corre, pero sin notarizar
fi
codesign --verify --deep --strict "$APP"

mkdir -p dist "$TMP/root"
cp -R "$APP" "$TMP/root/"
ln -s /Applications "$TMP/root/Applications"
hdiutil create -volname "OpenMix $VERSION" -srcfolder "$TMP/root" -ov -format UDZO "$DMG" >/dev/null
echo "$DMG  ($(du -h "$DMG" | cut -f1))"
shasum -a 256 "$DMG"
