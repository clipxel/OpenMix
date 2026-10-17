# OpenMix para iPhone y Android — diseño

Estado: **borrador de diseño** (octubre 2026). Nada de esto está implementado todavía.

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

Solo horizontal. Referencia: iPhone 15/16 (852×393 pt) y Android 20:9 (~ 800×360 dp).
Toques mínimos de 44 pt; nada depende del hover.

```
┌──────────────────────────────────────────────────────────────────────────┐
│ ● OpenMix   124.0 ⟷ 128.0     REC  ⚙              🔋 batería · CPU       │  barra 28
├──────────────────────────────────────────────────────────────────────────┤
│ A ▁▂▅▇▅▂▁▁▂▅▇█▇▅▂▁▁▂▅▇▅▂▁│▁▂▅▇▅▂▁▁▂▅▇█▇▅▂▁▁▂▅▇▅▂▁▁▂▅▇▅▂▁▁▂▅▇█▇▅▂▁▁▂▅▇▅▂ │  ondas
│ B ▁▁▂▅▇▅▂▁▁▂▅▇█▇▅▂▁▁▂▅▇▅│▂▁▁▂▅▇█▇▅▂▁▁▂▅▇▅▂▁▁▂▅▇▅▂▁▁▂▅▇█▇▅▂▁▁▂▅▇▅▂▁▁▂▅▇ │  2 × 52
├────────────────────────┬────────────────────────┬────────────────────────┤
│ Tema A · Artista   8A  │  HI  ◯      ◯  HI      │ Tema B · Artista   9A  │
│ 124.0  −1:32           │  MID ◯      ◯  MID     │ 128.0  −3:05           │
│ [1][2][3][4]  pads     │  LOW ◯      ◯  LOW     │ [1][2][3][4]  pads     │
│ [5][6][7][8]           │  FLT ◯      ◯  FLT     │ [5][6][7][8]           │
│                        │     ┃        ┃         │                        │
│ (CUE) (▶) (SYNC) LOOP  │     ┃fader   ┃fader    │ LOOP (SYNC) (▶) (CUE)  │
│ pitch ═══════●═══      │  ◀──────●──────▶ xfade │ pitch ═══●═══════      │
└────────────────────────┴────────────────────────┴────────────────────────┘
          ↑ deslizar desde abajo: biblioteca (hoja)        ↕ pestañas: PADS · FX · LOOP
```

- **Ondas arriba**, como en escritorio: tocar = saltar, arrastrar = scratch/nudge (en pausa,
  arrastrar mueve la cue).
- **Decks**: pads en una grilla 4×2 con pestañas (*Hot cue · Loop · Roll · Sampler*), los mismos
  modos que en escritorio. Sin jog (no entra); el arrastre sobre la onda lo reemplaza.
- **Mixer**: EQ de 3 bandas + filtro por canal como perillas que se arrastran en vertical
  (doble toque = centro), faders de canal y crossfader abajo.
- **Biblioteca**: hoja que sube desde abajo (búsqueda, playlists, tonalidad compatible
  resaltada). Arrastrar un tema hacia el deck A o B lo carga; también botones ◀A  B▶.
- **FX y AutoMix**: hojas laterales.
- **Multitáctil**: hay que poder mover el crossfader y un fader a la vez → cada control sigue
  su propio dedo (JUCE entrega cada toque como una fuente de mouse distinta; verificar que
  ningún control capture todas las fuentes).

En **tablets** (iPad, tablets Android ≥ 10") se usa el layout de escritorio actual con el
`ScaledHost`, toques agrandados y biblioteca como hoja si no entra.

---

## 5. Música en el teléfono

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

## 6. Fases

| Fase | Entregable | Cómo se prueba |
|---|---|---|
| **0 — Compila** | CMake para iOS, `OpenMix.jucer` para Android, guardias de plataforma, CI (`.github/workflows/movil.yml`) que genera el `.ipa` sin firmar y el `.apk` autofirmado | Instalar el `.ipa` con SideStore y el `.apk` a mano; se ve la UI de escritorio escalada |
| **1 — Suena** | Sesión de audio, segundo plano, interrupciones, modo cable dividido, carpeta propia de música | Mezclar dos temas con auriculares por cable Y |
| **2 — Se toca** | `Source/UI/Mobile`: layout de teléfono, hojas de biblioteca/FX, multitáctil | Sesión de 30 min solo con el teléfono |
| **3 — Completo** | MIDI por USB/BLE, interfaces USB de 4 salidas, pendrives, grabación y compartir, actualizador en Android | Controladora + interfaz |
| **4 — Publicado** | `.ipa`/`.apk` en Releases, fuente de SideStore, Obtainium, página de la web con instrucciones paso a paso (Modo desarrollador, flujo avanzado de Android) | Instalación desde cero siguiendo solo la web |

## 7. Preguntas abiertas

- ¿Hay una Mac disponible para probar en el simulador de iOS? (El CI compila igual en
  `macos-14`, pero depurar en el teléfono es mucho más cómodo con Xcode.)
- ¿iPad desde el principio o después del teléfono?
- Versión mínima: iOS 15 / Android 8 propuesto; subirla si simplifica algo.
- ¿Un solo `.apk` arm64 alcanza? (Cubre prácticamente todos los teléfonos actuales.)

## Fuentes (estado a octubre 2026)

- Flujo avanzado de Android para apps sin verificar: <https://9to5google.com/2026/08/18/google-gradually-rolling-out-androids-advanced-sideloading-ahead-of-developer-verification/>
- Fechas de la verificación y excepción de ADB: <https://www.androidauthority.com/google-android-advanced-flow-sideloading-rollout-begins-3700073/>
- SideStore / AltStore con Apple ID gratuito: <https://www.xda-developers.com/how-to-sideload-apps-on-iphone/>
