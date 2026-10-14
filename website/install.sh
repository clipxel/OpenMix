#!/bin/sh
# Instala (o actualiza) OpenMix en macOS desde la Terminal:
#   curl -fsSL https://clipxel.github.io/OpenMix/install.sh | sh
# Baja la última versión publicada, verifica su SHA-256 y copia OpenMix.app a Aplicaciones.
set -eu

REPO="clipxel/OpenMix"
MANIFEST="https://github.com/${REPO}/releases/latest/download/update.json"

say() { printf '\033[1m%s\033[0m\n' "$*"; }
fail() { printf 'Error: %s\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = "Darwin" ] || fail "este instalador es para macOS. Para Windows y Linux: https://clipxel.github.io/OpenMix/"

TMP=$(mktemp -d)
MNT="${TMP}/mnt"
cleanup() { hdiutil detach "${MNT}" -quiet -force >/dev/null 2>&1 || true; rm -rf "${TMP}"; }
trap cleanup EXIT INT TERM

say "Buscando la última versión…"
curl -fsSL "${MANIFEST}" -o "${TMP}/update.json" || fail "no se pudo leer la versión publicada (¿hay internet?)"
field() { plutil -extract "$1" raw -o - "${TMP}/update.json" 2>/dev/null; }
VERSION=$(field version) || fail "información de versión inválida"
URL=$(field assets.mac.url) || fail "no hay versión para macOS"
SHA=$(field assets.mac.sha256) || fail "falta la verificación de la descarga"
case "${URL}" in "https://github.com/${REPO}/releases/download/"*) ;; *) fail "dirección de descarga inesperada" ;; esac

say "Descargando OpenMix ${VERSION}…"
curl -fL --progress-bar "${URL}" -o "${TMP}/OpenMix.dmg" || fail "la descarga falló"
[ "$(shasum -a 256 "${TMP}/OpenMix.dmg" | cut -d' ' -f1)" = "${SHA}" ] || fail "el archivo descargado no coincide con el publicado"

mkdir -p "${MNT}"
hdiutil attach -nobrowse -readonly -quiet -mountpoint "${MNT}" "${TMP}/OpenMix.dmg" || fail "no se pudo abrir la imagen"
[ -d "${MNT}/OpenMix.app" ] || fail "la imagen no contiene OpenMix.app"

DEST="/Applications"
[ -w "${DEST}" ] || { DEST="$HOME/Applications"; mkdir -p "${DEST}"; }
pkill -x OpenMix 2>/dev/null && sleep 1 || true
rm -rf "${DEST}/OpenMix.app"
ditto "${MNT}/OpenMix.app" "${DEST}/OpenMix.app"

say "Listo: OpenMix ${VERSION} está en ${DEST}. Abriéndolo…"
open "${DEST}/OpenMix.app"
