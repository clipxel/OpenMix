#include "MapAssistant.h"

#include <set>

namespace om
{

namespace
{
/* How DJs name each function. One line = one way to say it: groups separated by ';' must all be
   there (a group starting with '?' only adds when it is), alternatives separated by '|', words of an
   alternative in order. A word ending in '*' matches any word that starts with it ("borr*": borrar,
   borre, borralo…); other words match exactly. Words are written as normalise() leaves them (no
   accents, numbers as digits). In the function, {n} is the number said (1…8); @loop, @roll,
   @jumpback and @jumpfwd pick the function by the size said ("loop de 8 beats", "saltar 16 atrás"). */
struct Way { const char* action; const char* spec; };

#define HOT  "hot cue|hotcue|hot cues|hotcues|hot|cue caliente|punto* de hot cue|marca*|memoria*|cue point*|cuepoint*|hot q|hotq|hc"
#define DEL  "borr*|elimin*|quit*|sac*|delet*|clear*|remov*|limpi*|eras*|suprim*|chau"
#define LOOPW "loop*|lup*|bucle*|lazo*|ciclo*"
#define FX   "fx|efecto*|effect*|efx"
#define BFX  "beatfx|beat efecto*|efecto* de beat|beat effect*|efecto* beat|efecto* del mixer|efecto* del mezclador|efectos master"
#define HEAD "auricular*|headphone*|cascos|casco|phones|phone|monitor*|vincha|audifono*|auris"
#define SELV "mostrar|muestre|ver|vea|pasar|pase|pasa|cambiar|cambie|cambia|seleccion*|select*|elegir|elija|ir|vaya|switch*|activar deck|swap*"
#define UPW  "sub*|up|aument*|mas|raise|arriba|incremen*|plus"
#define DOWNW "-|baj*|down|dismin*|menos|lower|abajo|reduc*|minus|decremen*"
#define TEMPO "pitch|pich|pitch fader|tempo|velocidad|speed|rate|bpm"
#define BACKW "atras|back|backward*|antes|retroced*|rebobin*|anterior|previo|rev|izq atras"
#define HOT_PN "hot cue|hotcue|cue|punto*|marca*"
#define FWDW "adelante|forward|fwd|despues|avanz*|siguiente|next|ff|futuro"

const Way ways[] =
{
    // ── Transport ──
    { "play_pause",        "play|plei|pley|reproduc*|arranc*|pausa*|pause|pausar|start|stop|par|parar|pare|para el tema|deten*|darle play|play pausa|play pause|playpause|tocar el tema|que suene|empez*" },
    { "cue_button",        "cue|cu|kiu|cue normal|boton cue|cue principal|punto de cue principal|punto de cue|punto cue" },
    { "goto_cue_stop",     "volv*|vuelv*|regres*|ir|return|retorn*;cue;par*|stop|deten*|quiet*|freno|frene" },
    { "stutter",           "stutter play|stuter play|stutter|stuter|tartamud*|estuter" },
    { "stutter",           "volv*|vuelv*|regres*|ir|return|desde|retorn*;cue;segu*|sigue|continu*|play|reproduc*|sin parar" },
    { "start_play",        "principio|inicio|comienzo|start of track|beginning|primer segundo|desde cero" },
    { "start_play",        "volv*|vuelv*|ir|vaya|regres*|return|go;principio|inicio|comienzo|start|beginning" },
    { "sync",              "sync|syn|sinc|sincro*|sinck|sink|synk|sinq|syncro|syncron*|beatmatch*|beat match*|empareja* bpm|iguala* bpm|iguala* tempo|igual* velocidad|match* bpm|match* tempo|empatar bpm" },
    { "master",            "master|maestro|lider|leader|guia;?sync|bpm" },
    { "master",            "hacer*|poner|elegir|marcar;master|maestro|lider" },
    { "reverse",           "revers*|al reves|invert*|marcha atras|reverso|rewind|hacia atras|para atras|backwards|retrocede la musica" },
    { "reverse",           "reproduc*|play|sonar|suene|tocar|toque|pase;reves|atras|revers*" },
    { "censor",            "censor*|censur*|bleep*|insulto*|malas palabras|puteada*|groseria*|tapar palabra*|pitido" },
    { "brake",             "brake|freno*|fren*|frenado|vinyl stop|stop de vinil*|apagado de bandeja|parada de vinil*|stop lento|apagar el plato|frenada" },
    { "backspin",          "backspin|back spin|spinback|spin back|giro atras|giro para atras|vuelta atras|rebobinado|spin" },
    { "slip_mode",         "slip|desliz*|resbal*|slip mode|modo slip|sleep" },
    { "quantize",          "quantiz*|cuantiz*|quantis*|cuantis*|quant|q|cuanti*|quanti*|cuantizado" },
    { "load",              "carg*|load*|meter tema|meter el tema|meter cancion|subir tema|subir el tema|cargue" },
    { "unload",            "expuls*|descarg*|eject*|unload*|sacar tema|sacar el tema|vaciar|vacie|quitar tema|quitar el tema|eyect*|sacar la cancion" },
    { "instant_doubles",   "instant double*|doubles|duplic* tema|duplic* deck|copiar deck|copiar el deck|copiar el otro|copia del otro|clon*|clone*|mismo tema en los dos|doble instantaneo" },
    { "emergency_play",    "play|boton|stop;emergencia|emergency|panic*|panico" },
    { "emergency_play",    "emergencia|emergency|panic*|panico|silencio de emergencia" },
    { "slip_mode",         "modo;slip|desliz*" },

    // ── Key ──
    { "key_lock",          "key lock|keylock|master tempo|mastertempo|master key|tono fijo|tonalidad fija|kl|key lok" },
    { "key_lock",          "bloq*|lock*|fij*|manten*|trab*|conserv*;tono|tonalidad|key|clave|nota" },
    { "key_up",            UPW ";semitono*|tono|tonalidad|key|clave|nota|semi tono" },
    { "key_down",          DOWNW ";semitono*|tono|tonalidad|key|clave|nota|semi tono" },
    { "key_tone_up",       UPW ";tonoentero" },
    { "key_tone_down",     DOWNW ";tonoentero" },
    { "key_reset",         "reset*|original|restabl*|volver|vuelva|cero|0|normal|devolver|quitar el cambio;tono|tonalidad|key|clave|nota" },
    { "key_match",         "igual*|match*|empareja*|sync|sincro*|armoniz*|misma|mismo|ajust*|compatib*;tono|tonalidad|key|clave|nota|armonia" },
    { "key_match",         "key sync|keysync|sync de tono|sync de tonalidad|key match" },

    // ── Jog and pitch ──
    { "jog",               "jog|jogwheel|jog wheel|plato|platos|rueda grande|bandeja|platter|wheel|rueda|jogs|yog|scratch*|scrach*|escrach*|rayar" },
    { "jog_touch",         "jog|plato|rueda|bandeja|platter|wheel;toque|toc*|touch|sensor*|tactil|apoy*|arriba del|superficie|encima" },
    { "jog_search",        "busc*|search*|rapido|avance rapido|fast|recorr*|navegar el tema;jog|plato|rueda|bandeja|wheel|tema|cancion" },
    { "needle_search",     "needle|aguja|tira*|strip|touch strip|barra tactil|tira tactil|needle drop" },
    { "vinyl_mode",        "vinil*|vinyl|vynil|vinyil|modo scratch|modo cd|cd mode|vinyl mode" },
    { "pitch",             TEMPO ";?fader|slider|desliz*|barra|regulador|control|palanca|perilla" },
    { "pitch_bend_minus",  "bend*|nudge*|empuj*|empuje|frenar un poco|retras*|frenadita|tirar para atras|tocar el borde|pitch bend|pitchbend;?" DOWNW "|lento|lenta|atras|despacio|negativo|-" },
    { "pitch_bend_plus",   "bend*|nudge*|empuj*|aceler* un poco|pitch bend|pitchbend;" UPW "|rapido|rapida|adelante|positivo|+" },
    { "pitch_bend_minus",  TEMPO ";" DOWNW ";poquito|poco|toque|mientras|momentane*|temporal*" },
    { "pitch_bend_plus",   TEMPO ";" UPW ";poquito|poco|toque|mientras|momentane*|temporal*" },
    { "pitch_bend_minus",  "fren*|retras*|atras*|frenalo;un poco|un toque|momentaneo|temporal*|mientras|poquito|de a poco" },
    { "pitch_bend_plus",   "aceler*|adelant*|apur*;un poco|un toque|momentaneo|temporal*|mientras|poquito|de a poco" },
    { "pitch_reset",       TEMPO ";reset*|cero|0|centro|centrar|original|resete*|normal|volver|vuelva|100" },
    { "pitch_minus",       TEMPO ";" DOWNW "|lento|menos|despacio|frenar" },
    { "pitch_plus",        TEMPO ";" UPW "|rapido|mas|aceler*" },
    { "all_pitch_minus",   TEMPO ";" DOWNW "|lento|menos;ambos|los dos|todos|both|all|dos decks|ambos decks" },
    { "all_pitch_plus",    TEMPO ";" UPW "|rapido|mas;ambos|los dos|todos|both|all|dos decks|ambos decks" },
    { "pitch_range",       "rango*|range|escala|ancho|amplitud|porcentaje;?" TEMPO },

    // ── Hot cues and loops ──
    { "hot_cue_{n}",       HOT ";?@n" },
    { "hot_cue_{n}",       "cue;@n" },
    { "hot_cue_{n}",       "punto*|marca*;?cue;@n" },
    { "delete_hot_cue_{n}", DEL ";punto* de cue|punto* cue|" HOT "|cue;?@n" },
    { "saved_loop_{n}",    LOOPW ";guardad*|memori*|salvad*|saved|almacen*|stored|grabad*|guarde|memorizad*;?@n" },
    { "loop",              LOOPW ";?auto|automatico|on|activ*|prend*|toggle|encend*|automatic*" },
    { "loop",              "auto loop|autoloop|auto lup" },
    { "@loop",             LOOPW ";@size" },
    { "@loop",             "@size;loop*|repeti*|bucle*" },
    { "loop_half",         "achic*|acort*|divid*|partir;mitad|medio|1/2" },
    { "loop_half",         LOOPW ";mitad|medio|1/2|half|divid*|achic*|reduc*|acort*|mas corto|menos|partir|dividilo|x1/2|1/2x" },
    { "loop_double",       LOOPW ";doble|x2|duplic*|agrand*|alarg*|double|mas largo|aument*|mas|multiplic*|2x" },
    { "loop_in",           LOOPW "|punto;in|entrada|inicio|comienzo|empiez*|empez*|arranque|principio" },
    { "loop_in",           "loop in|loopin|loop-in|in point|punto in" },
    { "loop_out",          LOOPW "|punto;out|salida|fin|final|end|cierre|termina" },
    { "loop_out",          "loop out|loopout|loop-out|out point|punto out" },
    { "loop_exit",         "salir|sali|salga|exit|apagar|apague|desactiv*|cort*|terminar|termine|cancel*|finaliz*;" LOOPW },
    { "reloop",            "reloop|re loop|re-loop|relup|volver al loop|vuelva al loop|repetir el loop|retomar el loop|reactiv* el loop|ultimo loop|loop anterior|reactivar loop|retomar loop" },
    { "loop_move_back",    LOOPW ";mov*|muev*|desplaz*|corr*|move*|llev*|shift*;" BACKW },
    { "loop_move_fwd",     LOOPW ";mov*|muev*|desplaz*|corr*|move*|llev*|shift*;" FWDW },
    { "loop_resize",       LOOPW ";@ctl:knob" },
    { "loop_resize",       LOOPW ";perilla|encoder|rueda|tamano|size|largo|longitud|medida|duracion|knob|ruedita|cambiar tamano" },
    { "loop_4",            "loop de 4|loop 4|4 beat loop|loop 4 beats" },
    { "beatjump_back",     "beat jump|beatjump|salto|salt*|jump|skip*|brinc*|beatjamp;" BACKW },
    { "beatjump_fwd",      "beat jump|beatjump|salto|salt*|jump|skip*|brinc*|beatjamp;" FWDW },
    { "beatjump_back",     "atrasar|retroceder|rebobinar|volver atras;?beat*|tiempo*|compas*" },
    { "beatjump_fwd",      "adelantar|avanzar;?beat*|tiempo*|compas*" },
    { "@jumpfwd",          "adelant*|avanz*;@size" },
    { "@jumpback",         "atras*|retroced*|rebobin*;@size" },
    { "@jumpback",         "beat jump|beatjump|salto|salt*|jump|skip*|brinc*;@size;?" BACKW },
    { "@jumpfwd",          "beat jump|beatjump|salto|salt*|jump|skip*|brinc*;@size;" FWDW },
    { "loop_roll_4",       "roll|rol|loop roll|rolls|rollo|roleo|repique" },
    { "@roll",             "roll|rol|loop roll|rolls|rollo|roleo|repique;@size" },

    // ── Mixer ──
    { "gain",              "gain|ganancia|trim|preamp*|sensibilidad|ganan*|gan|level de entrada|nivel de entrada|input level" },
    { "eq_high",           "agudo*|alto*|high*|treble|hi|brillo|tiple*|aguda*|platillos|hihat|hi hat" },
    { "eq_mid",            "medio*|mid|mids|media*|middle|voces" },
    { "eq_low",            "grave*|bajo|bajos|bass|low|lows|sub|bombo|graves" },
    { "eq_high",           "eq|ecualiz*|equaliz*|equalizer;agudo*|alto*|high*|treble|hi" },
    { "eq_mid",            "eq|ecualiz*|equaliz*;medio*|mid|media*" },
    { "eq_low",            "eq|ecualiz*|equaliz*;grave*|bajo|bajos|bass|low" },
    { "filter",            "filtro*|filter*|filtr*|hpf|lpf|cutoff|corte de frecuencia|filtrar|filtrado|filtre|sweep filter" },
    { "volume",            "@ctl:fader" },
    { "volume",            "volumen|volume|vol|fader de canal|channel fader|nivel|level|fader del canal|fader vertical|linea|line fader|fader" },
    { "master_volume",     "volumen|volume|vol|nivel|level;master|general|salida|main|principal|total|maestro|sala|parlante*" },
    { "master_volume",     "master volume|master level|volumen master|main volume|volumen general" },
    { "headphone_volume",  HEAD ";?volumen|volume|vol|nivel|level|gain|ganancia" },
    { "headphone_mix",     HEAD ";mix|mezcla|cue mix|balance|master|cue/master|mixing|blend" },
    { "headphone_mix",     "cue mix|cuemix|cue/master|mezcla cue|mix de cue|cue master" },
    { "pfl",               "cue auricular*|auricular* cue|pfl|preescucha|pre escucha|pre-escucha|escuchar|prescucha|pre listen|prelisten|headphone cue|cue auricular*|escuche|pre" },
    { "pfl",               HEAD ";cue|escuch*|boton|mand*|envi*|pre*|deck|canal|monitor*|send|@deckword|@left|@right" },
    { "switch_pfl",        "cambi*|switch*|altern*|pasar|pase|toggle;pfl|preescucha|pre escucha|" HEAD "|cue de auricular*" },
    { "crossfader",        "crossfader|cross fader|crossfeider|crosfader|crosfeder|crossfeder|xfader|x fader|cross|cruce|fader horizontal|crossfade" },
    { "crossfader_left",   "crossfader|cross fader|crossfeider|crosfader|xfader|cross|cruce;@left" },
    { "crossfader_right",  "crossfader|cross fader|crossfeider|crosfader|xfader|cross|cruce;@right" },
    { "mix_now",           "mezclar ahora|mezcla ahora|mix now|mezcla automatica rapida|transicion|cruzar ahora|mixear ahora|que mezcle ahora|hacer la transicion" },

    // ── Pads ──
    { "pad_{n}",           "pad|pads|padcito|boton pad|pad numero|performance pad*;@n" },
    { "pad_mode_hot_cue",  "modo*|mode|pagina|page|cambiar a|pasar a|poner en|vista;" HOT },
    { "pad_mode_roll",     "modo*|mode|pagina|page|cambiar a|pasar a|poner en;roll|rol|loop roll|rolls" },
    { "pad_mode_loop",     "modo*|mode|pagina|page|cambiar a|pasar a|poner en;" LOOPW "|auto loop|beat loop" },
    { "pad_mode_saved_loop", "modo*|mode|pagina|page|cambiar a|pasar a|poner en;slicer|slice*|guardad*|saved|memori*" },
    { "pad_mode_jump",     "modo*|mode|pagina|page|cambiar a|pasar a|poner en;jump|beat jump|beatjump|salto*" },
    { "pad_mode_key",      "modo*|mode|pagina|page|cambiar a|pasar a|poner en;tono|tonalidad|key|keyboard|teclado|pitch play|tone play" },
    { "pad_mode_fx",       "modo*|mode|pagina|page|cambiar a|pasar a|poner en;" FX "|pad fx" },
    { "pad_mode_sampler",  "modo*|mode|pagina|page|cambiar a|pasar a|poner en;sampler|sample*|muestra*|sonido*" },
    { "pad_mode_sampler",  "sampler|samplers|sample|samples|muestras" },
    { "pad_mode_saved_loop", "slicer|slice|slices|rebanad*" },
    { "pad_page",          "pagina*|page|modo*|mode|banco|bank;?pad*;siguiente|next|cambi*|otra|proxima|altern*|rotar|ciclar" },

    // ── Deck FX ──
    { "effect_active_{n}", FX ";?on|off|activ*|prend*|apag*|encend*|toggle|boton|enable|disable;?@n" },
    { "effect_wet_{n}",    FX ";@ctl:knob;?@n" },
    { "effect_wet_{n}",    FX ";nivel|wet|mezcla|cantidad|dry wet|drywet|dry/wet|intensidad|profundidad|depth|level|mix|amount|cuanto;?@n" },
    { "effect_param_{n}",  FX ";beat*|param*|tiempo|time|valor|ajuste|parametro;?@n" },
    { "effect_select_{n}", FX ";siguiente|next|cambi*|elegir|elija|selecc*|select*|otro|proximo;?@n" },
    { "effect_beat_down",  FX ";beat|beats|tiempo*;" DOWNW "|izq*|anterior|<|menos" },
    { "effect_beat_up",    FX ";beat|beats|tiempo*;" UPW "|der*|siguiente|>|mas" },

    // ── BEAT FX (the mixer's, Pioneer style) ──
    { "beat_fx_on",        BFX ";?on|off|activ*|prend*|apag*|encend*|toggle|boton|enable" },
    { "beat_fx_on_deck",   BFX ";?on|off|activ*|prend*|apag*|encend*|toggle;@deckword|@left|@right" },
    { "beat_fx_level",     "level depth|level/depth|depth|profundidad" },
    { "beat_fx_level",     BFX ";level|nivel|depth|profund*|cantidad|mezcla|intensidad|wet|level/depth|amount|mix" },
    { "beat_fx_level_deck", BFX ";level|nivel|depth|profund*|cantidad|mezcla|intensidad|wet|level/depth;@deckword|@left|@right" },
    { "beat_fx_select",    BFX ";siguiente|next|cambi*|selecc*|elegir|elija|select*|otro|proximo;?efecto*|fx|siguiente|next|cambi*" },
    { "beat_fx_select_back", BFX ";anterior|previous|prev|atras|antes|back" },
    { "beat_fx_tap",       "tap|tapear|tapeo|marcar bpm|marcar el bpm|golpear bpm|tap tempo|taptempo" },
    { "beat_fx_auto",      BFX ";auto|automatico|bpm del tema" },
    { "beat_fx_time",      BFX ";time|tiempo|duracion|largo" },
    { "beat_fx_beat_down", BFX ";beat*|tiempo*;" DOWNW "|izq*|anterior|<|menos" },
    { "beat_fx_beat_up",   BFX ";beat*|tiempo*;" UPW "|der*|siguiente|>|mas" },
    { "beat_fx_channel_next", BFX ";canal*|channel|ch|ch select|destino;siguiente|cambi*|next|otro|rotar" },
    { "beat_fx_channel",   BFX ";canal*|channel|ch|ch select|destino|asign*;@deckword|@left|@right" },
    { "beat_fx_master",    BFX ";master|salida|general|main|todo" },
    { "color_fx_next",     "colorfx|sound color|efecto de color|efectos de color|color effect" },
    { "color_fx_next",     "colorfx|sound color|sound colour|efecto de color|color effect|efectos de color|color;siguiente|next|cambi*|otro|elegir" },
    { "color_fx_filter",   "colorfx|sound color|efecto de color|perilla de color|color|modo de perilla|perilla color;filtro|filter" },
    { "color_fx_space",    "colorfx|sound color|efecto de color|perilla de color|color|perilla;space|espacio|reverb" },
    { "color_fx_echo",     "colorfx|sound color|efecto de color|perilla de color|color|perilla;echo|eco|dub echo|dub" },
    { "color_fx_sweep",    "colorfx|sound color|efecto de color|perilla de color|color|perilla;sweep|barrido" },
    { "color_fx_noise",    "colorfx|sound color|efecto de color|perilla de color|color|perilla;noise|ruido" },
    { "color_fx_crush",    "colorfx|sound color|efecto de color|perilla de color|color|perilla;crush|crusher|bitcrush*|distorsion" },
    { "color_fx_space",    "space|espacio" },
    { "color_fx_echo",     "dub echo" },
    { "color_fx_crush",    "crush|bitcrush*|crusher" },
    { "color_fx_noise",    "noise" },
    { "color_fx_sweep",    "sweep" },

    // ── Decks 3 and 4 ──
    { "deck_swap",         "cambi*|altern*|swap*|switch*|pasar|toggle;deck|decks|plato|lado|capa|layer|decklayer" },
    { "deck_swap",         "decklayer|deck layer|capa de deck" },
    { "deck_select_{d}",   SELV ";@deckword" },
    { "deck_select_{d}",   "@deckword;@left|@right" },

    // ── Library ──
    { "browser_scroll",    "busc*|elegir|elija|naveg*|recorr*|mover*;temas|canciones|lista|tracks|bibliot*|playlist*|libreria;?temas|canciones|tracks|lista" },
    { "browser_scroll",    "naveg*|browse*|browser|recorr*|scroll*|moverse|subir y bajar|lista de temas|bibliot*|libreria|library|temas|canciones|playlist|explorador|buscador|selector|elegir tema|elegir temas|mover por la lista" },
    { "browser_show_folders", "ir|vaya|mostrar|abrir|ver|pasar|volv*|vuelv*;playlist*|carpeta*|folder*|listas|lista de playlist*|crates|crate|arbol|sidebar|lateral" },
    { "browser_show_songs", "ir|mostrar|abrir|ver|pasar|volv*|vuelv*|regres*;temas|canciones|songs|tracks|lista de temas|pistas" },
    { "browser_focus",     "pasar|pase|cambi*|altern*|toggle|ir;temas|canciones|tracks;playlist*|carpeta*|listas" },
    { "browser_focus",     "back|atras en la biblioteca|alternar|cambi*|pasar|toggle|foco|focus;?bibliot*|lista|panel|temas|playlist*|carpeta*" },

    // ── Global ──
    { "record",            "grab* mezcla|grab* set|grab* sesion|grab*|rec|record*|recording|grabacion" },
    { "shift",             "shift|shif|sift|chift|shitf|shft" },
    { "automix",           "automix|auto mix|mezcla automatica|dj automatico|auto dj|autodj|mezcla sola|piloto automatico" },

    // ── Controller extras ──
    { "goto_start",        "volv*|vuelv*|ir|regres*|return|go;principio|inicio|comienzo|start|beginning;sin arrancar|sin play|sin reproducir|pausa|pausado|parado|quieto" },
    { "hot_cue_prev",      HOT_PN ";anterior|previo|previous|prev|atras" },
    { "hot_cue_next",      HOT_PN ";siguiente|proximo|next|adelante" },
    { "fader_start",       "fader start|faderstart|arranque con el fader|arranque por fader|fader de arranque" },
    { "stop_time",         "stop time|tiempo de frenado|tiempo de freno|duracion del freno|velocidad del freno" },
    { "crossfader_curve",  "crossfader|cross|xfader|cruce;curva|curve|contour|contorno|forma" },
    { "effect_beats",      "beats|beat;@ctl:knob;" FX },
    { "waveform_zoom_in",  "zoom|acerc*|agrand*;?onda|waveform|forma de onda;?in|mas|+" },
    { "waveform_zoom_out", "zoom|alej*|achic*;?onda|waveform|forma de onda;out|menos|-|alej*" },
    { "automix_queue",     "load prep|loadprep|prep|cargar a la cola" },
    { "automix_queue",     "agreg*|sum*|mand*|add;automix|auto mix|lista de automix|cola|queue|lista automatica" },

    // ── Lights ──
    { "guide_tempo_faster", "luz|led|guia|indicador|lampara|lucecita;tempo|velocidad;aceler*|mas rapido|rapido|subir" },
    { "guide_tempo_slower", "luz|led|guia|indicador|lampara|lucecita;tempo|velocidad;fren*|mas lento|lento|bajar" },
    { "guide_tempo_ok",    "luz|led|guia|indicador|lampara|lucecita;tempo|velocidad;ok|igual*|bien|correcto" },
    { "guide_phase_behind", "luz|led|guia|indicador|lampara|lucecita;fase;atras*|retras*|behind" },
    { "guide_phase_ahead", "luz|led|guia|indicador|lampara|lucecita;fase;adelant*|ahead" },
    { "guide_phase_ok",    "luz|led|guia|indicador|lampara|lucecita;fase;ok|en fase|bien|igual*|correcto" },
    { "vu_meter",          "vu|vumetro|vu meter|vumeter|medidor*|nivel de luces|leds de nivel|meter|metro" },
    { "vu_master",         "vu|vumetro|vu meter|vumeter|medidor*|meter;master|general|salida|main" },
    { "end_warning",       "fin|final|termin*|end|ultimo* segundo*;aviso|warning|luz|alerta|avis*|led|parpade*" },
};

#undef HOT
#undef DEL
#undef LOOPW
#undef FX
#undef BFX
#undef HEAD
#undef SELV
#undef UPW
#undef DOWNW
#undef TEMPO
#undef BACKW
#undef FWDW
#undef HOT_PN

// Words that say nothing about the function ("quiero que este botón haga…").
const std::set<juce::String> stopWords
{
    "que", "quiero", "quisiera", "queria", "me", "gustaria", "haga", "hacer", "hace", "hagas", "sirva", "sirve", "sea", "para", "por",
    "el", "la", "lo", "los", "las", "de", "del", "al", "a", "en", "un", "una", "unos", "unas", "este", "esta", "ese", "esa", "esto",
    "eso", "aquel", "ruedita", "boton", "botoncito", "botones", "tecla", "teclita", "perilla", "perillita", "knob", "control", "cuando", "apreto",
    "aprieto", "apretar", "apretas", "toco", "muevo", "presiono", "presionar", "pulso", "pulsar", "y", "o", "mi", "mis", "se",
    "le", "les", "su", "sus", "the", "an", "of", "to", "for", "this", "that", "button", "when", "press", "i", "want", "it", "make", "do",
    "should", "my", "pueda", "poder", "puedo", "funcione", "como", "tipo", "onda", "osea", "bueno", "porfa", "favor", "algo", "cosa",
    "usar", "uso", "use", "asignar", "asigna", "asignale", "mapear", "mapea", "mapeale", "map", "poner", "pone", "pongas", "ponga",
    "con", "tenga", "tiene", "haya", "va", "vaya a", "is", "be", "with", "and", "or", "at", "from", "es", "son", "sea el",
    "dale", "hacelo", "quiero que", "necesito", "necesitaria", "seria", "deberia", "debe", "tiene que", "tendria", "fader", "slider",
    "ese", "aca", "aqui", "ahi", "alli", "esto", "todo", "toda", "pista", "track", "musica", "cancion", "tema", "song",
    "funcion", "funciones", "accion", "hago", "hacemos", "sirvan", "simplemente", "solamente", "solo", "nada", "mas que",
};

// Words that only say how the control behaves; they barely count against a reading.
const std::set<juce::String> softWords
{
    "activar", "activa", "active", "activo", "prender", "prenda", "prende", "encender", "encienda", "apagar", "apague", "on", "off",
    "toggle", "enable", "disable", "boton", "momentaneo", "mantener", "mantenido", "apretado", "presionado", "sostenido", "mientras",
    "deck", "plato", "canal", "lado", "lento", "rapido", "arranque", "arranca", "arrancar", "dispare", "dispara", "disparar",
    "haga", "active", "activen", "prenda", "controle", "controla", "controlar", "maneje", "maneja", "manejar", "mueva", "mueve",
    "beat", "tengo", "tenga", "apretado", "aprieto", "aprietes", "trigger", "control", "controls", "set", "toggles", "toggle",
    "should", "would", "lo", "la", "otro", "otra", "tambien",
};

juce::String plain (juce::String s)
{
    s = s.toLowerCase();
    static const char* const from[] = { "á", "é", "í", "ó", "ú", "ü", "ñ", "à", "è", "ì", "ò", "ù", "â", "ê", "î", "ô", "û", "ç" };
    static const char* const to[]   = { "a", "e", "i", "o", "u", "u", "n", "a", "e", "i", "o", "u", "a", "e", "i", "o", "u", "c" };
    for (size_t i = 0; i < std::size (from); ++i) s = s.replace (juce::String::fromUTF8 (from[i]), to[i]);
    s = s.replace (juce::String::fromUTF8 ("½"), " 1/2 ").replace (juce::String::fromUTF8 ("¼"), " 1/4 ")
         .replace (juce::String::fromUTF8 ("×"), "x").replace (juce::String::fromUTF8 ("−"), "-")
         .replace (juce::String::fromUTF8 ("⇄"), " ");
    return s;
}

struct Token { juce::String w; bool used = false; float weight = 1.0f; };

/** Edit distance (for typos: "sincronizr", "crosfeder"). */
int distance (const juce::String& a, const juce::String& b)
{
    const int n = a.length(), m = b.length();
    std::vector<int> prev ((size_t) m + 1), cur ((size_t) m + 1);
    for (int j = 0; j <= m; ++j) prev[(size_t) j] = j;
    for (int i = 1; i <= n; ++i)
    {
        cur[0] = i;
        for (int j = 1; j <= m; ++j)
            cur[(size_t) j] = juce::jmin (prev[(size_t) j] + 1, cur[(size_t) j - 1] + 1,
                                          prev[(size_t) j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1));
        std::swap (prev, cur);
    }
    return prev[(size_t) m];
}

/** Spelling changes of Spanish verb forms: busque / buscar, apague / apagar, empiece / empezar. */
juce::String spelling (const juce::String& w)
{
    return w.replace ("qu", "c").replace ("q", "c").replace ("gu", "g").replace ("z", "c");
}

bool wordMatches (const juce::String& patternIn, const juce::String& wordIn)
{
    if (patternIn == wordIn) return true;
    const auto pattern = spelling (patternIn), word = spelling (wordIn);
    if (pattern.endsWithChar ('*')) return word.startsWith (pattern.dropLastCharacters (1));
    if (word == pattern) return true;
    // A Spanish verb written as its infinitive matches its other forms: frenar → frene, frena, frenalo.
    if (pattern.length() >= 5 && (pattern.endsWith ("ar") || pattern.endsWith ("er") || pattern.endsWith ("ir")) && ! pattern.containsChar (' '))
    {
        const auto stem = pattern.dropLastCharacters (2);
        return word.startsWith (stem) && word.length() <= stem.length() + 5;
    }
    return false;
}

/** Every word the vocabulary knows, for correcting typos. */
const std::vector<juce::String>& vocabulary()
{
    static const std::vector<juce::String> v = []
    {
        std::set<juce::String> s;
        for (auto& w : ways)
            for (auto& g : juce::StringArray::fromTokens (w.spec, ";", ""))
                for (auto& alt : juce::StringArray::fromTokens (g.trimCharactersAtStart ("?"), "|", ""))
                    for (auto& word : juce::StringArray::fromTokens (alt, " ", ""))
                        if (! word.startsWithChar ('@') && word.length() >= 4) s.insert (word.trimCharactersAtEnd ("*"));
        return std::vector<juce::String> (s.begin(), s.end());
    }();
    return v;
}

bool known (const juce::String& word)
{
    for (auto& v : vocabulary()) if (word.startsWith (v) || v == word || spelling (word).startsWith (spelling (v))) return true;
    return stopWords.count (word) > 0 || softWords.count (word) > 0;
}

int numberWord (const juce::String& w)
{
    static const std::map<juce::String, int> n
    {
        { "uno", 1 }, { "primero", 1 }, { "primer", 1 }, { "primera", 1 }, { "one", 1 }, { "first", 1 },
        { "dos", 2 }, { "segundo", 2 }, { "segunda", 2 }, { "two", 2 }, { "second", 2 },
        { "tres", 3 }, { "tercero", 3 }, { "tercer", 3 }, { "tercera", 3 }, { "three", 3 }, { "third", 3 },
        { "cuatro", 4 }, { "cuarto", 4 }, { "cuarta", 4 }, { "four", 4 }, { "fourth", 4 },
        { "cinco", 5 }, { "quinto", 5 }, { "quinta", 5 }, { "five", 5 }, { "fifth", 5 },
        { "seis", 6 }, { "sexto", 6 }, { "sexta", 6 }, { "six", 6 }, { "sixth", 6 },
        { "siete", 7 }, { "septimo", 7 }, { "septima", 7 }, { "seven", 7 }, { "seventh", 7 },
        { "ocho", 8 }, { "octavo", 8 }, { "octava", 8 }, { "eight", 8 }, { "eighth", 8 },
        { "dieciseis", 16 }, { "sixteen", 16 }, { "treinta y dos", 32 }, { "treintaidos", 32 }, { "thirty two", 32 },
        { "sesenta y cuatro", 64 },
    };
    auto it = n.find (w);
    return it != n.end() ? it->second : (w.containsOnly ("0123456789") && w.isNotEmpty() ? w.getIntValue() : 0);
}
} // namespace

juce::String MapAssistant::normalise (const juce::String& phrase)
{
    auto s = " " + plain (phrase) + " ";
    for (auto [a, b] : { std::pair { " un cuarto ", " 1/4 " }, { " cuarto de beat ", " 1/4 beat " }, { " un octavo ", " 1/8 " },
                         { " octavo de beat ", " 1/8 beat " }, { " medio beat ", " 1/2 beat " }, { " medio tiempo ", " 1/2 beat " },
                         { " half beat ", " 1/2 beat " }, { " quarter beat ", " 1/4 beat " }, { " un dieciseisavo ", " 1/16 " } })
        s = s.replace (a, b);
    juce::String out;
    for (auto c : s)
    {
        if (juce::CharacterFunctions::isLetterOrDigit (c) || c == '/') out << c;
        else if (c == '+') out << " mas ";
        else if (c == '-') out << " - ";
        else out << ' ';
    }
    // "x 2" / "2 x" → x2; "1 / 2" → 1/2
    auto words = juce::StringArray::fromTokens (out, " ", "");
    words.removeEmptyStrings();
    for (auto& w : words) { const int n = numberWord (w); if (n > 0 && ! w.containsOnly ("0123456789")) w = juce::String (n); }
    auto out2 = " " + words.joinIntoString (" ") + " ";
    out2 = out2.replace (" x 2 ", " x2 ").replace (" 2 x ", " x2 ").replace (" / ", "/");
    return out2.trim();
}

std::vector<MapAssistant::Reading> MapAssistant::interpret (const juce::String& phrase, int count) const
{
    auto text = " " + normalise (phrase) + " ";

    // Phrases that are one idea: whole tone, both decks, the deck-layer button (1/3, 2/4).
    for (auto [a, b] : { std::pair { " 2 semitonos ", " tonoentero " }, { " tono entero ", " tonoentero " }, { " whole tone ", " tonoentero " },
                         { " tono completo ", " tonoentero " }, { " los 2 ", " ambos " }, { " las 2 ", " ambos " }, { " both ", " ambos " },
                         { " deck 1 y deck 3 ", " decklayer " }, { " deck 2 y deck 4 ", " decklayer " }, { " deck 1 y 3 ", " decklayer " },
                         { " deck 2 y 4 ", " decklayer " }, { " deck 1 o deck 3 ", " decklayer " }, { " deck 2 o deck 4 ", " decklayer " },
                         { " color fx ", " colorfx " }, { " fader start ", " faderstart " }, { " rueda chica ", " ruedita " }, { " rueda pequena ", " ruedita " }, { " sound color fx ", " colorfx " }, { " 1/3 ", " decklayer " }, { " 2/4 ", " decklayer " }, { " 1 y 3 ", " decklayer " }, { " 2 y 4 ", " decklayer " },
                         { " 1 o 3 ", " decklayer " }, { " 2 o 4 ", " decklayer " }, { " beat fx ", " beatfx " }, { " beat efecto ", " beatfx " },
                         { " beat efectos ", " beatfx " }, { " efecto de beat ", " beatfx " }, { " efectos de beat ", " beatfx " },
                         { " beat effect ", " beatfx " } })
        text = text.replace (a, b).replace (a, b);

    // SHIFT layer: "con shift", "shift + play", "manteniendo shift", "segunda función".
    bool shift = false, saysShift = false;
    for (auto* s : { " shift ", " shif ", " sift ", " chift ", " shitf ", " shft " })
        if (text.contains (s)) { saysShift = true; text = text.replace (s, " "); }
    for (auto* s : { " 2 funcion ", " second function ", " capa 2 ", " 2 capa " })
        if (text.contains (s)) { shift = true; text = text.replace (s, " "); }

    // Deck: "deck 2", "plato dos", "canal 3", "ch1", "deck A", "izquierdo", "derecha".
    int deck = -2; bool saysDeck = false, left = false, right = false;
    auto words = juce::StringArray::fromTokens (text, " ", "");
    words.removeEmptyStrings();
    std::vector<Token> tokens;
    for (int i = 0; i < words.size(); ++i)
    {
        const auto& w = words[i];
        static const juce::StringArray deckWords { "deck", "decks", "plato", "canal", "channel", "ch", "player", "reproductor", "bandeja", "lado" };
        if (deckWords.contains (w) && i + 1 < words.size())
        {
            const auto& nx = words[i + 1];
            const int n = nx.length() == 1 && nx[0] >= 'a' && nx[0] <= 'd' ? nx[0] - 'a' + 1 : numberWord (nx);
            if (n >= 1 && n <= 4) { deck = n - 1; saysDeck = true; ++i; tokens.push_back ({ "@deckword" }); continue; }
        }
        if (w.length() >= 3 && w.startsWith ("ch") && w.substring (2).containsOnly ("1234")) { deck = w.substring (2).getIntValue() - 1; saysDeck = true; tokens.push_back ({ "@deckword" }); continue; }
        if (w.startsWith ("izquierd") || w == "left" || w == "izq" || w == "izquier") { left = true; tokens.push_back ({ "@left" }); continue; }
        if (w.startsWith ("derech") || w == "right" || w == "der") { right = true; tokens.push_back ({ "@right" }); continue; }
        tokens.push_back ({ w });
    }
    if (! saysDeck && (left || right)) deck = left ? 0 : 1;
    // "del deck 1 al deck 3": the two decks of one side → the deck-layer button.
    {
        std::vector<int> said;
        for (int i = 0; i + 1 < words.size(); ++i)
            if (words[i] == "deck" || words[i] == "plato")
                if (const int n = numberWord (words[i + 1]); n >= 1 && n <= 4) said.push_back (n - 1);
        if (said.size() >= 2 && said[0] != said[1] && said[0] % 2 == said[1] % 2)
        {
            int first = -1;
            for (size_t i = 0; i < tokens.size(); ++i) if (tokens[i].w == "@deckword") { if (first < 0) { first = (int) i; tokens[i].w = "decklayer"; } else tokens[i].w = "@gone"; }
            tokens.erase (std::remove_if (tokens.begin(), tokens.end(), [] (const Token& t) { return t.w == "@gone"; }), tokens.end());
            deck = -2; saysDeck = false;
        }
    }

    // Sizes: "8 beats", "1/2", "medio beat", "2 compases".
    double size = 0.0;
    for (size_t i = 0; i < tokens.size(); ++i)
    {
        auto& w = tokens[i].w;
        const auto next = i + 1 < tokens.size() ? tokens[i + 1].w : juce::String();
        const bool beatWord = next.startsWith ("beat") || next.startsWith ("tiempo") || next == "bt" || next == "bts" || next == "golpe" || next == "golpes" || next == "negras";
        const bool barWord = next.startsWith ("compas") || next == "bars" || next == "bar" || next == "barras";
        const auto prev = i > 0 ? tokens[i - 1].w : juce::String();
        if (w.matchesWildcard ("*/*", true) && w.upToFirstOccurrenceOf ("/", false, false).containsOnly ("0123456789")
            && ! (! beatWord && (prev.startsWith ("loop") || prev.startsWith ("lup") || prev.startsWith ("achic") || prev.startsWith ("achiq")
                                 || prev.startsWith ("acort") || prev.startsWith ("divid") || prev.startsWith ("part"))))     // "loop 1/2" is the LOOP ½ button
        {
            const double a = w.upToFirstOccurrenceOf ("/", false, false).getDoubleValue(), b = w.fromFirstOccurrenceOf ("/", false, false).getDoubleValue();
            if (b > 0) { size = a / b; w = "@size"; if (beatWord) tokens[i + 1].w = "@sizeunit"; }
        }
        else if ((w == "medio" || w == "half") && (beatWord || next.isEmpty())) { if (beatWord) { size = 0.5; w = "@size"; tokens[i + 1].w = "@sizeunit"; } }
        else if (w == "4" && (next == "de" || next.startsWith ("beat")) && false) {}
        else if (w.containsOnly ("0123456789") && w.isNotEmpty() && (beatWord || barWord))
        {
            size = w.getDoubleValue() * (barWord ? 4.0 : 1.0); w = "@size"; tokens[i + 1].w = "@sizeunit";
        }
    }
    tokens.erase (std::remove_if (tokens.begin(), tokens.end(), [] (const Token& t) { return t.w == "@sizeunit"; }), tokens.end());

    // "loop de 16", "saltar 8 atrás": a bare number next to a loop / roll / jump is its size.
    auto has = [&tokens] (std::initializer_list<const char*> stems)
    {
        for (auto& t : tokens) for (auto* st : stems) if (t.w.startsWith (st)) return true;
        return false;
    };
    if (size <= 0.0 && has ({ "loop", "lup", "roll", "rol", "salt", "jump", "beatjump", "skip" }) && ! has ({ "guardad", "memori", "saved", "salvad", "pad" }))
        for (auto& t : tokens)
            if (t.w.containsOnly ("0123456789") && t.w.isNotEmpty() && juce::StringArray { "1", "2", "4", "8", "16", "32", "64" }.contains (t.w))
            {
                size = t.w.getDoubleValue(); t.w = "@size"; break;
            }

    // The number said for numbered functions (hot cue 3, pad 5, FX 2).
    int number = 0;
    for (auto& t : tokens)
        if (t.w.containsOnly ("0123456789") && t.w.isNotEmpty() && t.w.getIntValue() >= 1 && t.w.getIntValue() <= 8) { number = t.w.getIntValue(); t.w = "@n"; break; }

    // Typos: a word the vocabulary does not know, one letter away from one it knows.
    for (auto& t : tokens)
        if (t.w.length() >= 5 && ! t.w.startsWithChar ('@') && ! known (t.w))
        {
            juce::String best; int bestD = 99;
            for (auto& v : vocabulary())
                if (v.length() >= 4)
                {
                    const int d = distance (t.w.substring (0, juce::jmax (v.length(), t.w.length())), v);
                    const int dPrefix = t.w.length() > v.length() ? distance (t.w.substring (0, v.length()), v) : 99;
                    const int dd = juce::jmin (d, dPrefix + 1);
                    if (dd < bestD) { bestD = dd; best = v; }
                }
            if (bestD <= (t.w.length() >= 8 ? 2 : 1)) t.w = best;
        }

    // Kind of control named: "perilla", "fader", "jog", "encoder", "botón".
    juce::String control;
    for (auto& w : words)
    {
        if (w == "perilla" || w == "knob" || w == "potenciometro" || w == "pote" || w == "dial" || w == "perillita") control = "knob";
        else if (w == "fader" || w == "slider" || w == "deslizador" || w == "palanca") control = "fader";
        else if (w == "encoder" || w == "ruedita" || w == "infinita") control = "encoder";
        else if ((w == "nivel" || w == "level" || w == "cantidad" || w == "intensidad") && control.isEmpty()) control = "knob";
        else if (w == "boton" || w == "tecla" || w == "pulsador" || w == "button" || w == "botoncito" || w == "pad") control = control.isEmpty() ? "button" : control;
    }

    // Content words left (stop words removed); the readings are scored by how much of them they explain.
    std::vector<Token> content;
    for (auto& t : tokens) if (! stopWords.count (t.w)) content.push_back (t);

    // "shift + play que haga stutter": the words after "que haga / sea / para" say the function; the ones
    // before name the control. The function's words count more.
    {
        static const juce::StringArray markers { "haga", "hace", "hacer", "sea", "que", "para", "funcione", "=", "sirva" };
        int marker = -1, idx = 0;
        for (auto& t : tokens) { if (markers.contains (t.w)) marker = idx; ++idx; }
        if (marker > 0)
        {
            int before = 0, after = 0; idx = 0;
            for (auto& t : tokens) { if (! stopWords.count (t.w)) (idx < marker ? before : after)++; ++idx; }
            if (before > 0 && after > 0)
            {
                std::vector<Token> weighted; idx = 0;
                for (auto& t : tokens)
                {
                    if (! stopWords.count (t.w)) { auto c = t; c.weight = idx < marker ? 0.5f : 2.0f; weighted.push_back (c); }
                    ++idx;
                }
                content = weighted;
            }
        }
    }

    if (juce::SystemStats::getEnvironmentVariable ("OM_INTENT_DEBUG", {}).isNotEmpty())
    { juce::String d; for (auto& t : content) d << t.w << "(" << t.weight << ") "; DBG (d); std::fprintf (stderr, "%s\n", d.toRawUTF8()); }
    struct Scored { Reading r; float score; int groups; };
    std::vector<Scored> found;

    auto tryWay = [&] (const Way& way)
    {
        auto toks = content;
        int groups = 0;
        float virtualCover = 0.0f;     // "@ctl:fader": the kind of control alone says the function
        for (auto g : juce::StringArray::fromTokens (way.spec, ";", ""))
        {
            const bool optional = g.startsWithChar ('?');
            g = g.trimCharactersAtStart ("?");
            if (g.isEmpty()) continue;
            bool hit = false;
            // Longest alternative first, so "hot cue" wins over "hot".
            auto alts = juce::StringArray::fromTokens (g, "|", "");
            std::stable_sort (alts.begin(), alts.end(), [] (const juce::String& a, const juce::String& b) { return a.length() > b.length(); });
            for (auto& alt : alts)
            {
                if (alt.startsWith ("@ctl:"))
                {
                    const auto kind = alt.fromFirstOccurrenceOf (":", false, false);
                    if (control == kind || (kind == "knob" && control == "encoder")) { hit = true; virtualCover += 1.0f; break; }
                    continue;
                }
                auto seq = juce::StringArray::fromTokens (alt, " ", "");
                seq.removeEmptyStrings();
                for (int k = seq.size(); --k >= 0;) if (stopWords.count (seq[k])) seq.remove (k);   // "al reves" → "reves"
                if (seq.isEmpty()) continue;
                for (size_t i = 0; i + (size_t) seq.size() <= toks.size() && ! hit; ++i)
                {
                    bool all = true;
                    for (int k = 0; k < seq.size() && all; ++k)
                    {
                        auto& t = toks[i + (size_t) k];
                        all = ! t.used && (seq[k] == "@size" ? t.w == "@size" : wordMatches (seq[k], t.w));
                    }
                    if (all) { for (int k = 0; k < seq.size(); ++k) toks[i + (size_t) k].used = true; hit = true; }
                }
                if (hit) break;
            }
            if (! hit && ! optional) return;
            if (hit) ++groups;
        }

        // What is left unexplained counts against the reading.
        float covered = virtualCover, missed = 0.0f;
        bool numberLeft = false;
        for (auto& t : toks)
        {
            if (t.used) covered += t.weight * (softWords.count (t.w) ? 0.5f : 1.0f);
            else if (t.w == "@n") { numberLeft = true; missed += 0.15f; }
            else if (t.w == "@size") missed += 1.0f;                // a size said and not used: wrong function
            else if (t.w == "@deckword" || t.w == "@left" || t.w == "@right") missed += 0.15f;
            else missed += (softWords.count (t.w) ? 0.25f : 1.0f) * t.weight;
        }
        if (covered <= 0.0f) return;

        juce::String action = way.action;
        bool usedNumber = false;
        auto sizeIndex = [&] (std::initializer_list<double> sizes) -> int
        {
            int i = 1; for (auto s : sizes) { if (std::abs (s - size) < 1.0e-6) return i; ++i; } return 0;
        };
        if (action == "@loop")
        {
            if (std::abs (size - 4.0) < 1.0e-6) action = "loop_4";
            else if (const int i = sizeIndex ({ 0.25, 0.5, 1, 2, 4, 8, 16, 32 })) action = "pad_loop_" + juce::String (i);
            else return;
        }
        else if (action == "@roll")
        {
            if (const int i = sizeIndex ({ 0.125, 0.25, 0.5, 1 })) action = "loop_roll_" + juce::String (i);
            else if (const int k = sizeIndex ({ 1.0 / 32, 1.0 / 16, 0.125, 0.25, 0.5, 1, 2, 4 })) action = "pad_roll_" + juce::String (k);
            else return;
        }
        else if (action == "@jumpback" || action == "@jumpfwd")
        {
            const int base = action == "@jumpback" ? 0 : 4;
            if (const int i = sizeIndex ({ 1, 4, 8, 16 })) action = "pad_jump_" + juce::String (base + i);
            else if (const int k = sizeIndex ({ 32, 64 })) action = "pad_jump_long_" + juce::String (base + 2 + k);
            else return;
        }
        if (action.contains ("{n}"))
        {
            const bool numberSaid = std::any_of (toks.begin(), toks.end(), [] (const Token& t) { return t.w == "@n"; });
            action = action.replace ("{n}", juce::String (juce::jmax (1, number)));
            usedNumber = numberSaid;
            if (! numberSaid) missed += 0.4f;    // "hot cue" without a number: probably 1, but ask
        }
        if (action == "deck_select_{d}")
        {
            if (deck < 0) return;
            action = "deck_select_" + juce::String (deck + 1);
        }

        const auto* info = actions.find (action);
        if (info == nullptr) return;

        float score = covered / (covered + missed);
        // The kind of control the user names fits the function?
        if (control.isNotEmpty())
        {
            const auto t = info->type;
            const bool fits = (control == "button" && t == ControlType::button)
                           || (control == "fader" && t == ControlType::absolute)
                           || (control == "knob" && t != ControlType::button)
                           || (control == "encoder" && t == ControlType::relative);
            score += fits ? 0.12f : -0.2f;
        }
        score += 0.02f * (float) groups;          // more specific ways win ties
        juce::ignoreUnused (usedNumber);

        Reading r;
        r.action = action;
        r.label = info->label;
        r.deck = info->perDeck ? (deck >= 0 ? deck : -2) : -1;
        // "volumen 2", "channel fader 4": a number the function does not use, on a per-deck function, is the deck.
        if (info->perDeck && deck < 0 && numberLeft && ! usedNumber && number >= 1 && number <= 4) r.deck = number - 1;
        r.shift = shift;
        r.control = control;
        found.push_back ({ r, score, groups });
    };

    for (auto& w : ways) tryWay (w);

    // SHIFT itself: "este botón es el shift" (nothing else asked) or the layer of another function.
    if (saysShift)
    {
        const bool other = std::any_of (found.begin(), found.end(), [] (const Scored& s) { return s.score >= 0.35f; });
        if (other) for (auto& f : found) f.r.shift = true;
        else
        {
            Reading r; r.action = "shift"; r.label = "Shift"; r.deck = -1; r.control = control;
            found.push_back ({ r, 1.0f, 1 });
        }
    }

    // Nothing in the vocabulary: compare with the names of the functions.
    if (found.empty())
    {
        for (auto& info : actions.list())
        {
            auto label = juce::StringArray::fromTokens (normalise (info.label), " ", "");
            label.removeEmptyStrings();
            int hit = 0;
            for (auto& t : content) for (auto& l : label) if (l.length() >= 3 && (t.w == l || (l.length() >= 5 && t.w.startsWith (l.substring (0, 5))))) { ++hit; break; }
            if (hit > 0)
            {
                Reading r; r.action = info.id; r.label = info.label; r.deck = info.perDeck ? (deck >= 0 ? deck : -2) : -1; r.shift = shift || saysShift;
                found.push_back ({ r, 0.3f * (float) hit / (float) juce::jmax (1, (int) content.size()), 0 });
            }
        }
    }

    std::stable_sort (found.begin(), found.end(), [] (const Scored& a, const Scored& b) { return a.score > b.score; });
    std::vector<Reading> out;
    for (auto& f : found)
    {
        if (std::any_of (out.begin(), out.end(), [&] (const Reading& o) { return o.action == f.r.action; })) continue;
        auto r = f.r;
        r.score = juce::jlimit (0.0f, 1.0f, f.score);
        out.push_back (r);
        if ((int) out.size() >= count) break;
    }
    return out;
}

} // namespace om
