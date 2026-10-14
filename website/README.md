# Sitio web de OpenMix

HTML/CSS/JS sin frameworks + un servidor Python sin dependencias (`server.py`, Python 3.9+).
Estilo de sitio de producto de DJ (barra negra, bloques grises redondeados, bandas negras, botones píldora) con la identidad de OpenMix: logo, coral y colores de deck de `Source/UI/Theme/Theme.cpp`.

```bash
cd website
python3 server.py                     # http://localhost:8080
```

| Página | Qué hace |
|---|---|
| `index.html` | Funciones, capturas, atajos y descarga con detección de sistema (macOS / Windows / Linux / celular) |
| `mapeos.html` | Mapeos MIDI oficiales y de la comunidad: buscar, filtrar, descargar y subir `midi.json` |
| `foro.html` | Foro con categorías, temas y respuestas (enlaces compartibles `foro.html#t=<id>`) |

## Descargas

Las URLs se arman desde `config/downloads.json`: `repo` + `version` + `file` de cada plataforma
apuntan al release `v<versión>` de GitHub. Los instaladores (macOS universal, Windows y Linux) los
compila y sube `.github/workflows/release.yml` al publicar un tag; en local, `./scripts/package.sh`
arma el de macOS.

`index.html?descargar` inicia la descarga del sistema detectado automáticamente.

## Mapeos

Se acepta el `midi.json` que guarda OpenMix (`MidiEngine::save`): se valida tipo MIDI, codificación,
canal, número y acción de cada asignación antes de publicarlo.

- Comunidad: `data/mappings.json` + `data/mappings/<id>.json`.
- Oficiales: `official/index.json` + los archivos en `official/`. Para promover uno de la comunidad:

```bash
curl -X POST -H "X-Admin-Token: $OPENMIX_ADMIN_TOKEN" -H "Content-Type: application/json" -d '{}' http://localhost:8080/api/mappings/<id>/official
```

### Reconocimiento automático en la app (2.0)

Al enchufar un controlador sin mapeo, OpenMix descarga `official/index.json` (cacheado 12 h, sigue
andando sin conexión) y compara **en la computadora** el nombre MIDI del dispositivo con `match.midi`.
Si coincide, baja el archivo del mapeo y lo instala. No se envía ningún dato del usuario ni del equipo.

```json
{
  "version": 2,
  "mappings": [{
    "id": "modelo-x", "name": "Modelo X", "file": "modelo-x.midi.json",
    "match": { "midi": ["Modelo X"], "usb": ["1234:5678"] },
    "driver": { "windows": { "page": "https://…/descargas", "detect": ["%ProgramFiles%\\Fabricante\\driver.dll"] } }
  }]
}
```

- `match.midi`: nombre exacto del puerto (se ignoran números de puerto y el prefijo "2- " de Windows);
  terminado en `*` acepta toda la familia ("Modelo X *" también reconoce "Modelo X MK2").
- `file`: solo el nombre del archivo, en esta misma carpeta.
- `driver.<windows|mac|linux>` (opcional): si el controlador necesita un driver en ese sistema, `page` es la
  página oficial de descarga y `detect` las rutas que existen cuando ya está instalado. OpenMix **no descarga
  ni instala drivers**: si falta, avisa y ofrece un botón que abre esa página.

## Moderación

Arrancá el servidor con `OPENMIX_ADMIN_TOKEN=<secreto>`. Con el header `X-Admin-Token`:
`DELETE /api/mappings/<id>`, `DELETE /api/forum/threads/<id>`, `DELETE /api/forum/threads/<id>/replies/<rid>`,
y publicar en *Anuncios*. Detrás de un proxy (nginx, Caddy) usá `OPENMIX_TRUST_PROXY=1` para que el límite
de una publicación cada 20 s por IP use la IP real.

## Publicar en internet

`HOST=0.0.0.0 PORT=8080 python3 server.py` detrás de un proxy con HTTPS. Los datos de la comunidad
viven en `data/`: hacé copias de seguridad de esa carpeta.
