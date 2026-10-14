# OpenMix

Software de DJ para macOS, Windows y Linux (JUCE 8 / C++20), con un lenguaje visual minimalista e identidad propia.

## Descargar

Los instaladores están en la sección **Releases** del repositorio y en la página web:

| Sistema | Archivo | Primera vez |
|---|---|---|
| macOS 12+ (Apple Silicon e Intel) | `.dmg` universal | Si macOS avisa que no puede verificarla: *Ajustes del Sistema › Privacidad y seguridad › Abrir igualmente* |
| Windows 10/11 (64 bits) | instalador `.exe` | Se instala por usuario, con acceso en el menú Inicio y desinstalador; si aparece SmartScreen: *Más información › Ejecutar de todas formas* |
| Linux x86_64 | `.tar.gz` | Descomprimir y ejecutar `./instalar.sh` |

Las versiones de Windows y Linux las compila GitHub Actions (`.github/workflows/release.yml`) al
publicar un tag `v*`, y las sube solas al release junto con la de macOS.

## Compilar

```bash
./scripts/bootstrap.sh
open build/OpenMix_artefacts/Release/OpenMix.app
```

## Empaquetar una versión

La versión sale de `project(OpenMix VERSION …)` en `CMakeLists.txt`.

```bash
./scripts/package.sh        # compila, corre los autotests, firma y arma dist/OpenMix-<versión>-macOS-universal.dmg
```

Después: crear el release `v<versión>` en GitHub y adjuntar el `.dmg`. La página web
(`website/`, ver su README) se publica sola en GitHub Pages con cada push a `main`.

## Estructura

| Carpeta | Contenido |
|---|---|
| `Source/Engine` | Motor de audio: decks (pistas en memoria, interpolación Hermite, loops, cues, sync de tempo+fase), mixer (trim, EQ aislador Linkwitz-Riley con kill, filtro LPF/HPF, faders, crossfader con 3 curvas, PFL a salidas 3/4), FX (echo sincronizado, reverb), grabación WAV, análisis (waveform 3 bandas, BPM + beatgrid) |
| `Source/Library` | Biblioteca: base de datos JSON liviana, escaneo de carpetas, tags vía AudioToolbox, análisis en segundo plano, playlists, historial |
| `Source/UI/Theme` | Sistema de diseño: tokens (`Theme`), iconos propios, LookAndFeel |
| `Source/UI/Components` | Componentes reutilizables: Button, Knob, Fader, LevelMeter, StatusPill, Segmented |
| `Source/UI/Views` | TopBar, WaveformStrip/Overview, DeckView (jog), MixerView, LibraryView, FxPanel, SettingsPanel |

Los controles marcados con borde punteado y **N/I** todavía no están implementados (sampler).

## Funciones

- **Decks:** cue estilo reproductor de club, sync tempo+fase, 8 hot cues, loops (1/32–32), loops guardados, loop roll, beatjump,
  slip, reverse, brake, censor, scratch en modo vinilo, pitch hasta ±100 %, **key lock** y cambio de tono por
  semitonos (Signalsmith Stretch), key match.
- **Análisis:** BPM + beatgrid y **tonalidad en Camelot**, en segundo plano. La tonalidad compatible con lo que suena
  se resalta en la biblioteca.
- **Control:** registro único de acciones (`Source/Control/Actions`) compartido por teclado y MIDI. MIDI con MIDI Learn,
  SHIFT, 7/14 bits, encoders relativos, jog, soft takeover y LEDs (`Configuración › Controladores`).
- **Pendrives exportados para reproductores de DJ:** lectura de la base del export y de los archivos ANLZ
  (beatgrid, cues). **Solo lectura**: nunca se escribe en la carpeta del export ni en los archivos de audio. Las cues creadas en OpenMix para pistas de discos
  externos se guardan en `<disco>/.OpenMix/cues.json` (carpeta oculta, creada recién al guardar la primera cue).

## Atajos de teclado (por defecto)

| Deck 1 | Deck 2 | Acción |
|---|---|---|
| Z / X / C | M / , / . | Cue / Play / Sync |
| 1–4, Q W E R | 7–0, U I O P | Hot cues 1–8 (⇧ borra) |
| A / S / D | J / K / L | Loop / ½ / ×2 (⇧S ⇧D: beatjump) |
| F / G | [ / ] | Pitch bend − / + |
| T | Y | Loop roll ¼ |
| V | / | Brake |
| ⇧A / ⇧Z / ⇧C | ⇧J / ⇧M / ⇧. | Reverse / Censor / Slip |

↑ ↓ navegan la biblioteca, ⇧← ⇧→ cargan en deck 1 / 2, ⌘F busca, ⌘, abre Configuración.

## Tests

```bash
# Motor, análisis, MIDI, lector de pendrives exportados y cues en .OpenMix (sin dispositivo de audio)
OpenMix.app/Contents/MacOS/OpenMix --selftest --data /tmp/om --a a.wav --b b.wav --usb /ruta/a/export-de-prueba
# Inspección solo lectura de un pendrive real (verifica que la carpeta del export no cambie)
OpenMix.app/Contents/MacOS/OpenMix --inspect-usb /Volumes/MI-PENDRIVE
```

## Verificación sin ventana

```bash
build/OpenMix_artefacts/Release/OpenMix.app/Contents/MacOS/OpenMix --snapshot out.png --size 1440x860 --workspace performance --data /tmp/om-data --load-a a.wav --load-b b.wav
```

## Licencia

OpenMix es software libre bajo la [GNU Affero General Public License v3](LICENSE).
Usa [JUCE](https://juce.com) bajo AGPLv3 y [Signalsmith Stretch / Linear](https://signalsmith-audio.co.uk) bajo licencia MIT
(ver [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).
