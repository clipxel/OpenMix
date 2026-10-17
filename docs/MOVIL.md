# OpenMix para iPhone y Android — diseño

Estado (octubre 2026): **fase 0 en curso, solo iPhone.** El CI (`.github/workflows/ios.yml`) compila
un `.ipa` sin firmar con la interfaz de escritorio escalada. Android queda para cuando haya un
teléfono para probar.

## Probarlo en el iPhone (fase 0)

1. Bajar el `.ipa`: *Actions › iPhone ›* el último run verde *› Artifacts › OpenMix-…-iOS*
   (viene dentro de un `.zip`: descomprimirlo).
2. Instalar **SideStore** siguiendo <https://sidestore.io> (una sola vez, con una compu) o
   **AltStore** (<https://altstore.io>) con AltServer en la Mac/PC.
3. Pasar el `.ipa` al iPhone (AirDrop o Archivos) y abrirlo con SideStore/AltStore
   (*Mis apps › +*). Se firma con tu Apple ID gratuito.
4. La primera vez: *Ajustes › Privacidad y seguridad › Modo desarrollador* → activar y reiniciar.
5. Música: *Archivos › En mi iPhone › OpenMix* → copiar los temas ahí.

La firma dura 7 días; SideStore la renueva sola (AltStore, cuando el iPhone ve a AltServer en la
misma red).

Objetivo: que OpenMix corra en iPhone y en Android **sin firma digital de editor**, es decir, sin
pagar la cuenta de desarrollador de Apple (USD 99/año) y sin registrarse como desarrollador
verificado en Google. Mismo motor (JUCE 8 / C++20), misma identidad visual, interfaz táctil propia.

---

## 1. Cómo se instala sin firma de editor

### Android — fácil

Un `.apk` siempre tiene que estar firmado, pero **la firma puede ser nuestra** (un certificado
autogenerado, sin ninguna autoridad ni cuenta de Google). Lo que no hay es "firma de editor".

| Vía | Cómo | Notas |
|---|---|---|
| **APK desde Releases / la web** (principal) | Descargar `OpenMix-<versión>-Android.apk`, tocar, *Permitir apps de esta fuente* | Igual que hoy con el `.dmg` y el `.exe` |
| Obtainium | La app sigue los Releases de GitHub y actualiza sola | Cero infraestructura nuestra |
| F-Droid (repo propio o el oficial) | Más adelante | El oficial compila desde el código; exige build reproducible |
| `adb install` | Desde una compu | Exento de toda verificación |

**La clave de firma** se genera una sola vez (`keytool`) y se guarda como secreto de GitHub
Actions (`ANDROID_KEYSTORE_B64`, `ANDROID_KEYSTORE_PASS`). No se puede perder: Android solo acepta
una actualización si viene firmada con la misma clave. Guardar una copia fuera de GitHub.

**Ojo — verificación de desarrolladores de Google (2026–2027).** Google empezó a exigir que las
apps instaladas por fuera de Play vengan de desarrolladores verificados: desde el 30/09/2026 en
Brasil, Indonesia, Singapur y Tailandia, y en el resto del mundo a partir de 2027. Para apps sin
verificar quedan dos caminos:

- el **"flujo avanzado"**: *Opciones de desarrollador › Permitir apps de desarrolladores no
  verificados*, una espera única de 24 h, y después cada instalación muestra un aviso con
  *Instalar de todas formas*;
- `adb install`, que queda exento.

Decisión: **arrancar sin verificar** (APK autofirmado) y documentar el flujo avanzado en la web.
Si en 2027 la fricción molesta, evaluar la cuenta gratuita de "desarrollador aficionado" que
anunció Google (verificar condiciones en su momento): no es una firma de editor, solo registra la
clave que ya usamos.

### iPhone — posible, con límites

iOS no instala nada que no esté firmado por un certificado de Apple. Sin cuenta paga, lo que
existe es la **firma personal gratuita**: cada usuario firma la app con *su propio* Apple ID.

| Vía | Cómo | Límites |
|---|---|---|
| **SideStore** (principal) | Instalación única desde una compu; después firma y renueva en el propio iPhone | La firma vence a los 7 días y SideStore la renueva sola; máximo 3 apps por Apple ID gratuito |
| AltStore clásico + AltServer | AltServer en Mac/Windows renueva por Wi-Fi | Igual, pero necesita la compu cerca cada semana |
| Xcode con Apple ID gratuito | Compilar y correr desde una Mac | Para nosotros (desarrollo), no para usuarios |
| TrollStore | Instalación permanente sin firma | Solo versiones viejas de iOS (hasta 17.0): nicho |

Nosotros publicamos un **`.ipa` sin firmar** en Releases; SideStore/AltStore lo firman en el
teléfono de cada uno con su Apple ID. Además publicamos una **fuente de SideStore/AltStore**
(`website/altstore.json`, servida por GitHub Pages) para que la app aparezca en su catálogo y se
actualice sola.

Requisitos del lado del usuario: activar **Modo desarrollador** (*Ajustes › Privacidad y
seguridad*, iOS 16+) y confiar en su propio perfil la primera vez.

Lo que **no** sirve sin cuenta paga: TestFlight, App Store, distribución ad hoc y los
marketplaces alternativos de la UE (AltStore PAL exige notarización de Apple → cuenta paga).

Si algún día se quiere distribución "normal" en iPhone, el paso siguiente es la cuenta paga +
TestFlight; el `.ipa` es el mismo, solo cambia quién lo firma.

### ¿Y una web app (PWA)?

Se descartó como camino principal: implicaría reescribir el motor en Web Audio, y en iPhone
Safari **no tiene Web MIDI** (sin controladoras) ni salidas de audio separadas para el cue.
Puede servir más adelante como demo en la web, no como OpenMix.

---

## 2. Arquitectura: un solo código, tres destinos nuevos

```
Source/Engine, Library, Control  ──  sin cambios de fondo (C++ portable, ya corre en 3 SO)
Source/UI/Theme, Components      ──  se reutilizan; se agrandan los toques (≥ 44 pt)
Source/UI/Views                  ──  escritorio y tablet
Source/UI/Mobile   (nuevo)       ──  layout de teléfono: MobileMixView, MobileLibrarySheet…
Source/Platform    (nuevo)       ──  archivos, permisos, sesión de audio por plataforma
```

| Destino | Compilación | Notas |
|---|---|---|
| iOS / iPadOS 15+ | CMake con `-G Xcode -DCMAKE_SYSTEM_NAME=iOS`, `CODE_SIGNING_ALLOWED=NO` | El CMake de JUCE soporta iOS directo |
| Android 8+ (API 26), arm64-v8a | Proyecto Gradle generado por **Projucer** (`OpenMix.jucer` mínimo) + `Projucer --resave` en CI | El CMake de JUCE no empaqueta apps Android; Projucer genera el proyecto que adentro usa CMake |

Audio: JUCE usa **AVAudioSession** en iOS y **Oboe/AAudio** en Android (latencia baja).

### Qué hay que tocar en el código actual

Revisado en el código de hoy; casi todo es `#if JUCE_MAC … #elif JUCE_WINDOWS … #elif JUCE_LINUX`
que en iOS/Android cae al vacío:

| Dónde | Qué pasa | Qué hacer |
|---|---|---|
| `Library/MetadataReader_portable.cpp` | Se compila en todo lo que no es `JUCE_MAC`: en iOS chocaría con `MetadataReader_mac.mm` | Guardia `! (JUCE_MAC \|\| JUCE_IOS)`; el `.mm` (AudioToolbox) sirve en iOS tal cual |
| `CMakeLists.txt` | Universal x86_64/arm64 y MP3 por JUCE solo fuera de Darwin | En iOS: solo arm64, decodificar MP3/AAC con el SO; en Android: `JUCE_USE_MP3AUDIOFORMAT=1` |
| `Library/Library.cpp` `dataFolder()` | En iOS/Android `userApplicationDataDirectory` es el contenedor de la app | Correcto; solo cambiar el comentario |
| `Library/Volumes.cpp`, `DeviceMonitor` | No hay `/Volumes` ni `/media` | Fase 3: pendrives por selector de documentos (iOS) / SAF (Android) |
| `UI/Views/LibraryView.cpp` (`mdfind`, carpetas Música/Escritorio) | No existen en móvil | Fuentes móviles: carpeta de la app, "Archivos", biblioteca de audio del sistema |
| `revealToUser()` ("Mostrar en Finder") | No aplica | Ocultar en móvil; en su lugar "Compartir" |
| `Common/Updater.cpp` (`ChildProcess`, scripts) | No hay procesos hijos | iOS: desactivado (actualiza SideStore). Android: descargar APK y abrir el instalador del sistema |
| `Control/KeyboardControl` | Sin teclado físico casi siempre | Sigue funcionando con teclado Bluetooth/USB; nada más |
| `Main.cpp` `MainWindow` / `ScaledHost` | Ventana redimensionable con barra nativa | En móvil: pantalla completa, solo horizontal, respetar *safe areas* (isla/notch) |
| `MainComponent::resized()` | Pensado para ≥ 960×560 (escalado 0,72 como mínimo) | Teléfono → layout nuevo (§4); tablet → el de escritorio, con toques más grandes |
| `TooltipWindow`, hover | No hay mouse | Pulsación larga = tooltip / menú contextual |

---

## 3. Audio en el teléfono: el problema del cue

Un DJ necesita dos salidas: master (parlantes) y cue (auriculares). Hoy el PFL va a las salidas
3/4. En el teléfono hay tres modos, elegibles en *Configuración › Audio*:

1. **Interfaz USB** (iPhone USB-C / Lightning con adaptador, Android con OTG): si la interfaz
   tiene 4 salidas, igual que en la compu (master 1/2, cue 3/4). Funciona con interfaces
   *class compliant*; en Android depende del teléfono → marcar como "a probar".
2. **Cable dividido** (por defecto sin interfaz): master en mono por el canal izquierdo y cue en
   mono por el derecho, con un cable Y. Es lo que hacen las apps de DJ móviles. Implementación:
   un modo más en `AudioEngine` al armar el buffer de salida.
3. **Solo master**: sin cue (para mezclar por la salida del teléfono o Bluetooth). Advertir que
   el Bluetooth agrega latencia (~150–250 ms) y que no sirve para pre-escuchar.

Sesión de audio: categoría *playback* en iOS (suena con el silencioso activado), audio en
segundo plano (`UIBackgroundModes: audio`) para que no se corte al bloquear la pantalla, y
manejo de interrupciones (llamadas) → pausar con fade, no cortar.

Rendimiento: dos decks + key lock (Signalsmith) + análisis. Medir en un teléfono de gama media;
si hace falta, en móvil el análisis corre con prioridad baja y solo con la app en primer plano,
y el key lock usa la configuración "económica" de Signalsmith.

---

## 4. Interfaz de teléfono (horizontal)

**No es la versión de PC achicada**: es una interfaz propia, pensada para los dedos. Los bocetos
de las cinco pantallas están en el lienzo de diseño (Claude Design, "OpenMix Mobile").

Solo horizontal. Teléfono de referencia: **iPhone 16 Pro** (874×402 pt en horizontal, USB-C), el
que se usa para probar. También tiene que entrar en el iPhone de 6,1" común (852×393) y en
Android 20:9 (~ 800×360 dp). Se dibuja dentro de la *safe area* (~60 pt a cada lado por la isla). Toques de 44 pt como
mínimo en lo que se toca mientras suena (play, cue, pads, carga en A/B); nada depende del hover.
Mismos colores, iconos y tipografía que en escritorio (`Source/UI/Theme`).

### 4.1 Mezcla — tocando en la pantalla

```
┌──────────────────────────────────────────────────────────────────────────┐
│ ● OpenMix  [Sin controladora]        MASTER 124.0  [Biblioteca] (●) (⚙)  │
│ A ▁▂▅▇▅▂▁▁▂▅▇█▇▅▂▁▁▂▅▇▅▂▁│▁▂▅▇▅▂▁▁▂▅▇█▇▅▂▁▁▂▅▇▅▂▁▁▂▅▇▅▂▁▁▂▅▇█▇▅▂▁▁▂▅▇▅▂ │
│ B ▁▁▂▅▇▅▂▁▁▂▅▇█▇▅▂▁▁▂▅▇▅│▂▁▁▂▅▇█▇▅▂▁▁▂▅▇▅▂▁▁▂▅▇▅▂▁▁▂▅▇█▇▅▂▁▁▂▅▇▅▂▁▁▂▅▇ │
├──┬─────────────────────┬─────────────────┬─────────────────────────┬──┤
│+ │ Tema A        8A 124.0│ ◯  HI   ◯       │124.0 9A        Tema B   │ +│
│▕ │[HOT CUE|LOOP|ROLL|SMP]│ ◯  MID  ◯       │[HOT CUE|LOOP|ROLL|SMP]  │ ▏│
│█ │ [1][2][3][4]          │ ◯  LOW  ◯       │          [1][2][3][4]   │ █│
│▕ │ [5][6][7][8]          │ ◯  FLT  ◯       │          [5][6][7][8]   │ ▏│
│▕ │ (CUE)(▶)[SYNC] LOOP 4 │ ┃  ▮▮▮▮  ┃       │ LOOP 8 [SYNC](▶)(CUE)   │ ▏│
│− │                       │ ◀────●────▶     │                         │ −│
└──┴─────────────────────┴─────────────────┴─────────────────────────┴──┘
 pitch A + pitch bend (borde izq.)                 pitch B + pitch bend (borde der.)
```

- **Espejado**: el deck B es el reflejo del A (play y cue hacia afuera, al alcance del pulgar);
  el pitch va en el borde exterior de cada lado, como en una controladora.
- **Pitch bend + / −**: arriba y abajo de cada pitch. Mientras se mantiene apretado, el tema
  se adelanta (+) o se atrasa (−) para emparejar los golpes a oído; al soltar vuelve a la
  velocidad del pitch. Usan las acciones que ya existen (`pitch_bend_plus` / `pitch_bend_minus`,
  las mismas de F/G y [/] en el teclado y de las controladoras).
- **Ondas**: tocar = saltar; arrastrar = scratch (en vinilo) o nudge; en pausa, arrastrar mueve
  la cue. Pellizcar = zoom. Reemplazan al jog, que no entra.
- **Pads 4×2** con pestañas *Hot cue · Loop · Roll · Sampler* (los mismos modos de escritorio).
  Mantener apretado un hot cue = borrarlo (en lugar de ⇧).
- **Mixer**: EQ de 3 bandas + filtro por canal (arrastrar en vertical; doble toque = centro;
  el kill se marca en color), faders de canal con vúmetros y crossfader.
- **Multitáctil**: crossfader y fader a la vez, o dos pads a la vez. Cada control sigue su
  propio dedo (en JUCE cada dedo es una fuente de mouse distinta).
- **Barra**: estado de la controladora, BPM master, biblioteca, grabar y configuración.

### 4.2 Biblioteca — hoja que sube

Deslizar desde abajo o tocar *Biblioteca*. Arriba quedan a la vista los dos decks (tema, BPM,
tiempo restante) para no perder la mezcla. Búsqueda por texto/BPM/tonalidad, filtro
*Compatibles con A/B* (tonalidad Camelot y BPM cercano), fuentes a la izquierda (todos, en el
iPhone, playlists, historial, Archivos…, pendrive) y en cada fila los botones **A** y **B** para
cargar. Bajar la hoja = volver a la mezcla.

### 4.3 Con controladora conectada

Cuando se conecta una controladora con mapeo, **el celular pasa a ser la pantalla de la
controladora**: se ocultan los controles táctiles y quedan ondas grandes (con loops y cues
marcados), el estado de cada deck en letra grande (tema, BPM, tiempo, tonalidad, pitch, sync,
key lock) y la biblioteca, que se recorre con la perilla de la controladora. Un botón
*Mostrar controles en pantalla* vuelve a la vista 4.1 para quien quiera usar las dos cosas.

### 4.4 Conexiones

Una sola pantalla para lo que hay que enchufar: controladora (USB detectada sola; Bluetooth
MIDI con *Emparejar*), mapeo cargado, salida de audio (§3) y latencia.

### 4.5 Tablets

En iPad y tablets Android de 10" o más entra el layout de escritorio (`ScaledHost`), con
toques agrandados y la biblioteca como hoja si no entra.

---

## 5. Controladoras de DJ en el celular

Es una de las funciones principales de la versión móvil.

| | iPhone | Android |
|---|---|---|
| USB | USB-C (como el iPhone 16 Pro): directo o con hub; las controladoras chicas se alimentan del iPhone, las grandes con su fuente o un hub con alimentación. Lightning: adaptador de cámara USB 3 **con carga** (el iPhone no alimenta controladoras grandes) | USB-C con OTG; controladoras grandes: hub con alimentación |
| Bluetooth MIDI | Emparejar desde OpenMix (JUCE abre el diálogo del sistema) | Igual, con la API MIDI de Android |
| Placa de audio de la controladora | Si es *class compliant*: master a parlantes y cue a auriculares de la controladora (4 salidas) | Igual; depende del teléfono → "a probar" |
| Mapeos | Los mismos `midi.json` de escritorio, la misma detección automática por nombre | Ídem |

Lo que ya existe y se reutiliza tal cual: `Control/MidiEngine` (MIDI Learn, SHIFT, 7/14 bits,
encoders, jog, soft takeover, LEDs), `ControllerLibrary` (mapeos oficiales) y el registro de
acciones. Lo nuevo: el emparejamiento Bluetooth, la detección de controladora conectada para
cambiar de vista (4.3) y elegir sola la placa de la controladora como salida de audio.

---

## 6. Actualizaciones de la versión móvil

**Las versiones de celular van aparte de las de PC.** Tienen su propio número y sus propios
releases, y salen cuando hay cambios para el celular (no con cada versión de escritorio).

| | PC | Celular |
|---|---|---|
| Versión | `project(OpenMix VERSION …)` | `OPENMIX_MOBILE_VERSION` en `CMakeLists.txt` |
| Tag del release | `v2.1.0` | `movil-v0.1.0` |
| Lo compila | `release.yml` | `ios.yml` (y `android.yml` más adelante) |
| De dónde se actualiza | `releases/latest/download/update.json` | `releases/download/movil/source.json` (dirección fija) |

Los releases del celular se crean con `--latest=false`, así "latest" sigue siendo el de PC y el
actualizador de escritorio no se confunde.

**Cómo se saca una versión de celular**

1. Subir `OPENMIX_MOBILE_VERSION` en `CMakeLists.txt` (ej. `0.2.0`).
2. Escribir las novedades en `.github/release-notes/movil-v0.2.0.md`.
3. Crear y subir el tag `movil-v0.2.0`. El CI compila el `.ipa`, crea el release y agrega la
   versión a `source.json`.

**Cómo le llega al usuario — iPhone**

- `source.json` es una **fuente de SideStore/AltStore**: se agrega una vez en SideStore
  (*Fuentes › +*, con la dirección del release `movil`) y desde ahí SideStore ve cada versión
  nueva, la instala firmada con el Apple ID del usuario y sigue renovando la firma.
- Además la app lee la misma fuente al abrirse y, si hay una versión más nueva, muestra la
  tarjeta *OpenMix Mobile 0.x · Actualizar con SideStore* (solo con los dos decks parados,
  nunca en medio de una mezcla). El toque abre SideStore con la versión lista.
- Lo que iOS no deja: que una app se reinstale sola sin pasar por SideStore. "Se actualiza
  sola" en iPhone = aviso en la app + un toque.

**Cómo le llega al usuario — Android** (cuando exista la versión)

- La app lee su manifiesto, descarga el `.apk`, verifica el SHA-256 (como el actualizador de
  escritorio) y abre el instalador del sistema: Android pide confirmar con un toque.
- Alternativa sin abrir la app: Obtainium sigue los releases `movil-v*` y actualiza en segundo
  plano.

## 7. Música en el teléfono

| | iPhone | Android |
|---|---|---|
| Carpeta propia | "Archivos › En mi iPhone › OpenMix" (`UIFileSharingEnabled` + `LSSupportsOpeningDocumentsInPlace`): copiar música desde la compu o desde Archivos | `Android/media/app.openmix/` o carpeta elegida |
| Elegir archivos sueltos | Selector de documentos (JUCE `FileChooser`) → se copia a la carpeta propia | `FileChooser` (SAF, URLs `content://`) → se copia o se lee por `juce::URL` |
| Biblioteca del sistema | Música de Apple: solo temas sin DRM (los de Apple Music no se pueden leer) — fase posterior | Permiso `READ_MEDIA_AUDIO` y escaneo de la carpeta Música |
| Pendrives / exports para reproductores | Fase 3, vía Archivos (solo lectura, igual que hoy) | Fase 3, vía SAF |

La base JSON de la biblioteca y las cues viven en el contenedor de la app, igual que hoy en
`dataFolder()`.

Controladoras MIDI: JUCE soporta CoreMIDI en iOS (USB y Bluetooth MIDI) y la API MIDI de Android
(USB y BLE). Los mapeos `midi.json` son los mismos que en escritorio.

---

## 8. Fases

| Fase | Entregable | Cómo se prueba |
|---|---|---|
| **0 — Compila** | ✅ CMake para iOS, guardias de plataforma, `ios.yml` con `.ipa` sin firmar y canal `movil-v*` · Android: `OpenMix.jucer` + `.apk` autofirmado, cuando haya teléfono | Instalar el `.ipa` con SideStore; se ve la UI de escritorio escalada |
| **1 — Suena** | Sesión de audio, segundo plano, interrupciones, modo cable dividido, carpeta propia de música | Mezclar dos temas con auriculares por cable Y |
| **2 — Se toca** | `Source/UI/Mobile`: layout de teléfono, hojas de biblioteca/FX, multitáctil | Sesión de 30 min solo con el teléfono |
| **3 — Controladoras** | Vista "con controladora" (4.3), Bluetooth MIDI, placa de la controladora como salida (4 salidas), pantalla de Conexiones | Mezclar 30 min con una controladora enchufada al iPhone |
| **4 — Completo y publicado** | Tarjeta de actualización en la app, pendrives, grabación y compartir; página de la web con instrucciones paso a paso (SideStore, Modo desarrollador) | Instalación y actualización desde cero siguiendo solo la web |

El canal de actualizaciones del celular (tags `movil-v*`, `source.json`) ya está armado en el
CI desde la fase 0.

## 9. Preguntas abiertas

- Teléfono de prueba: iPhone 16 Pro (USB-C).
- ¿Hay una Mac disponible para probar en el simulador de iOS? (El CI compila igual en
  `macos-15`, pero depurar en el teléfono es mucho más cómodo con Xcode.)
- Android: sin teléfono para probar, queda en pausa (se puede usar el emulador de Android Studio
  para lo básico, pero no sirve para medir latencia).
- ¿iPad desde el principio o después del teléfono?
- Versión mínima: iOS 15 / Android 8 propuesto; subirla si simplifica algo.
- ¿Un solo `.apk` arm64 alcanza? (Cubre prácticamente todos los teléfonos actuales.)

## Fuentes (estado a octubre 2026)

- Flujo avanzado de Android para apps sin verificar: <https://9to5google.com/2026/08/18/google-gradually-rolling-out-androids-advanced-sideloading-ahead-of-developer-verification/>
- Fechas de la verificación y excepción de ADB: <https://www.androidauthority.com/google-android-advanced-flow-sideloading-rollout-begins-3700073/>
- SideStore / AltStore con Apple ID gratuito: <https://www.xda-developers.com/how-to-sideload-apps-on-iphone/>
