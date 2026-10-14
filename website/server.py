#!/usr/bin/env python3
"""Servidor del sitio de OpenMix — sin dependencias (solo la biblioteca estándar de Python 3.9+).

Sirve los archivos estáticos de esta carpeta y una API JSON para:
  · Mapeos de controladores: los oficiales (official/) y los que sube la comunidad.
  · Foro: categorías, hilos y respuestas.

Datos de la comunidad en data/ (mappings.json, forum.json, mappings/<id>.json).
Moderación: definí OPENMIX_ADMIN_TOKEN y mandá el header X-Admin-Token en las rutas de admin.

    python3 server.py                 # http://localhost:8080
    PORT=9000 python3 server.py
"""

import json
import os
import re
import secrets
import threading
import time
from datetime import datetime, timezone
from http import HTTPStatus
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

ROOT = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(ROOT, "data")
UPLOADS = os.path.join(DATA, "mappings")
OFFICIAL = os.path.join(ROOT, "official")
ADMIN_TOKEN = os.environ.get("OPENMIX_ADMIN_TOKEN", "")
TRUST_PROXY = os.environ.get("OPENMIX_TRUST_PROXY") == "1"

MAX_BODY = 600 * 1024           # pedido completo
MAX_MAPPING = 512 * 1024        # midi.json
MAX_BINDINGS = 4000
POST_INTERVAL = 20              # segundos entre publicaciones por IP

CATEGORIES = [
    {"id": "anuncios", "name": "Anuncios", "desc": "Novedades y versiones de OpenMix"},
    {"id": "ayuda", "name": "Ayuda", "desc": "Instalación, audio, biblioteca y dudas de uso"},
    {"id": "controladores", "name": "Controladores", "desc": "MIDI, mapeos y hardware"},
    {"id": "ideas", "name": "Ideas", "desc": "Propuestas de funciones"},
    {"id": "errores", "name": "Errores", "desc": "Reportes de bugs"},
    {"id": "tecnica", "name": "Técnica de mezcla", "desc": "Sets, transiciones y consejos"},
]
CATEGORY_IDS = {c["id"] for c in CATEGORIES}
ADMIN_ONLY_CATEGORIES = {"anuncios"}

KINDS = {"note", "cc", "pitchwheel"}
ENCODINGS = {"button", "abs7", "abs14", "rel_offset", "rel_twos", "rel_sign"}
ACTION_RE = re.compile(r"^[a-z][a-z0-9_]{0,48}$")

lock = threading.Lock()
last_post = {}


def now():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def load(name, default):
    path = os.path.join(DATA, name)
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        return default


def save(name, value):
    os.makedirs(DATA, exist_ok=True)
    path = os.path.join(DATA, name)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(value, f, ensure_ascii=False, indent=1)
    os.replace(tmp, path)


def clean(value, limit, required=True, multiline=False):
    if not isinstance(value, str):
        value = ""
    value = value.replace("\r\n", "\n").strip()
    if not multiline:
        value = " ".join(value.split())
    value = "".join(ch for ch in value if ch == "\n" or ch >= " ")
    if required and not value:
        raise ValueError("Faltan campos obligatorios.")
    if len(value) > limit:
        raise ValueError(f"Texto demasiado largo (máximo {limit} caracteres).")
    return value


def validate_mapping(doc):
    """Comprueba que sea un midi.json de OpenMix (MidiEngine::save) y devuelve un resumen."""
    if not isinstance(doc, dict) or not isinstance(doc.get("bindings"), list):
        raise ValueError("El archivo no es un mapeo de OpenMix: falta la lista \"bindings\".")
    bindings = doc["bindings"]
    if not bindings:
        raise ValueError("El mapeo está vacío.")
    if len(bindings) > MAX_BINDINGS:
        raise ValueError("El mapeo tiene demasiadas asignaciones.")
    actions = set()
    for i, b in enumerate(bindings, 1):
        if not isinstance(b, dict):
            raise ValueError(f"Asignación {i} inválida.")
        if b.get("kind") not in KINDS:
            raise ValueError(f"Asignación {i}: tipo MIDI desconocido.")
        if b.get("encoding", "button") not in ENCODINGS:
            raise ValueError(f"Asignación {i}: codificación desconocida.")
        action = b.get("action")
        if not isinstance(action, str) or not ACTION_RE.match(action):
            raise ValueError(f"Asignación {i}: acción inválida.")
        number = b.get("number", 0)
        channel = b.get("channel", 1)
        if not isinstance(number, int) or not 0 <= number <= 127:
            raise ValueError(f"Asignación {i}: número MIDI fuera de rango.")
        if not isinstance(channel, int) or not 1 <= channel <= 16:
            raise ValueError(f"Asignación {i}: canal MIDI fuera de rango.")
        actions.add(action)
    devices = sorted({str(b.get("device", "*")) for b in bindings if b.get("device", "*") != "*"})[:4]
    return {"bindings": len(bindings), "actions": len(actions), "devices": devices}


def public_mapping(m):
    return {k: m[k] for k in ("id", "brand", "model", "name", "author", "description",
                              "bindings", "actions", "devices", "downloads", "created", "official")
            if k in m}


def official_mappings():
    try:
        with open(os.path.join(OFFICIAL, "index.json"), encoding="utf-8") as f:
            items = json.load(f).get("mappings", [])
    except FileNotFoundError:
        return []
    counts = load("official-downloads.json", {})
    out = []
    for m in items:
        m = dict(m, official=True, downloads=counts.get(m["id"], 0))
        out.append(m)
    return out


class Handler(SimpleHTTPRequestHandler):
    server_version = "OpenMixSite/1"

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=ROOT, **kwargs)

    # ── utilidades ──────────────────────────────────────────────────────────
    def send_json(self, value, status=HTTPStatus.OK):
        body = json.dumps(value, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def error_json(self, message, status=HTTPStatus.BAD_REQUEST):
        self.send_json({"error": message}, status)

    def read_json(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length <= 0:
            raise ValueError("Pedido vacío.")
        if length > MAX_BODY:
            raise ValueError("El pedido es demasiado grande.")
        try:
            return json.loads(self.rfile.read(length).decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            raise ValueError("JSON inválido.")

    def client_ip(self):
        # X-Forwarded-For solo vale detrás de un proxy propio (si no, cualquiera lo falsifica).
        if TRUST_PROXY and self.headers.get("X-Forwarded-For"):
            return self.headers["X-Forwarded-For"].split(",")[-1].strip()
        return self.client_address[0]

    def is_admin(self):
        token = self.headers.get("X-Admin-Token", "")
        return bool(ADMIN_TOKEN) and secrets.compare_digest(token, ADMIN_TOKEN)

    def throttle(self, body):
        if body.get("website"):            # campo trampa para bots
            raise ValueError("Pedido rechazado.")
        ip = self.client_ip()
        t = time.time()
        if t - last_post.get(ip, 0) < POST_INTERVAL and not self.is_admin():
            raise ValueError("Esperá unos segundos antes de publicar otra vez.")
        last_post[ip] = t
        if len(last_post) > 5000:
            for k in [k for k, v in last_post.items() if t - v > POST_INTERVAL]:
                del last_post[k]

    def end_headers(self):
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "same-origin")
        if not urlparse(self.path).path.startswith("/api/"):
            self.send_header("Cache-Control", "no-cache")   # revalida: una versión nueva se ve al instante
        super().end_headers()

    def translate_path(self, path):
        # data/ nunca se sirve como archivo estático: todo pasa por la API.
        clean_path = urlparse(path).path
        if clean_path.startswith("/data/") or clean_path == "/data" or clean_path.endswith(".py"):
            return os.path.join(ROOT, "__no_existe__")
        return super().translate_path(path)

    # ── rutas ───────────────────────────────────────────────────────────────
    def do_GET(self):
        url = urlparse(self.path)
        parts = [p for p in url.path.split("/") if p]
        if not parts or parts[0] != "api":
            return super().do_GET()
        q = {k: v[0] for k, v in parse_qs(url.query).items()}
        try:
            if parts[1:] == ["mappings"]:
                return self.list_mappings(q)
            if len(parts) == 4 and parts[1] == "mappings" and parts[3] == "download":
                return self.download_mapping(parts[2])
            if parts[1:] == ["forum", "categories"]:
                return self.categories()
            if parts[1:] == ["forum", "threads"]:
                return self.list_threads(q)
            if len(parts) == 4 and parts[1:3] == ["forum", "threads"]:
                return self.get_thread(parts[3])
        except IndexError:
            pass
        self.error_json("Ruta desconocida.", HTTPStatus.NOT_FOUND)

    def do_POST(self):
        parts = [p for p in urlparse(self.path).path.split("/") if p]
        try:
            body = self.read_json()
            if not isinstance(body, dict):
                raise ValueError("JSON inválido.")
            if parts == ["api", "mappings"]:
                return self.create_mapping(body)
            if parts == ["api", "forum", "threads"]:
                return self.create_thread(body)
            if len(parts) == 5 and parts[1:3] == ["forum", "threads"] and parts[4] == "replies":
                return self.create_reply(parts[3], body)
            if len(parts) == 4 and parts[1] == "mappings" and parts[3] == "official":
                return self.promote_mapping(parts[2])
        except ValueError as e:
            return self.error_json(str(e))
        self.error_json("Ruta desconocida.", HTTPStatus.NOT_FOUND)

    def do_DELETE(self):
        parts = [p for p in urlparse(self.path).path.split("/") if p]
        if not self.is_admin():
            return self.error_json("No autorizado.", HTTPStatus.FORBIDDEN)
        with lock:
            if len(parts) == 3 and parts[1] == "mappings":
                items = load("mappings.json", [])
                kept = [m for m in items if m["id"] != parts[2]]
                save("mappings.json", kept)
                try:
                    os.remove(os.path.join(UPLOADS, parts[2] + ".json"))
                except FileNotFoundError:
                    pass
                return self.send_json({"ok": len(kept) != len(items)})
            if len(parts) == 4 and parts[1:3] == ["forum", "threads"]:
                forum = load("forum.json", {"threads": []})
                before = len(forum["threads"])
                forum["threads"] = [t for t in forum["threads"] if t["id"] != parts[3]]
                save("forum.json", forum)
                return self.send_json({"ok": len(forum["threads"]) != before})
            if len(parts) == 6 and parts[1:3] == ["forum", "threads"] and parts[4] == "replies":
                forum = load("forum.json", {"threads": []})
                for t in forum["threads"]:
                    if t["id"] == parts[3]:
                        t["replies"] = [r for r in t["replies"] if r["id"] != parts[5]]
                save("forum.json", forum)
                return self.send_json({"ok": True})
        self.error_json("Ruta desconocida.", HTTPStatus.NOT_FOUND)

    # ── mapeos ──────────────────────────────────────────────────────────────
    def list_mappings(self, q):
        with lock:
            community = [dict(m, official=False) for m in load("mappings.json", [])]
        items = official_mappings() + community
        kind = q.get("type")
        if kind == "official":
            items = [m for m in items if m["official"]]
        elif kind == "community":
            items = [m for m in items if not m["official"]]
        text = q.get("q", "").lower().strip()
        if text:
            items = [m for m in items
                     if text in " ".join(str(m.get(k, "")) for k in ("brand", "model", "name", "author", "description")).lower()]
        # Oficiales primero; dentro de cada grupo, más descargados o más recientes.
        field = "downloads" if q.get("sort") == "downloads" else "created"
        items.sort(key=lambda m: m.get(field, 0 if field == "downloads" else ""), reverse=True)
        items.sort(key=lambda m: not m["official"])
        self.send_json({"mappings": [public_mapping(m) for m in items]})

    def download_mapping(self, mid):
        if not re.fullmatch(r"[a-z0-9-]{4,64}", mid):
            return self.error_json("Mapeo inexistente.", HTTPStatus.NOT_FOUND)
        with lock:
            official = next((m for m in official_mappings() if m["id"] == mid), None)
            if official:
                path = os.path.join(OFFICIAL, official["file"])
                counts = load("official-downloads.json", {})
                counts[mid] = counts.get(mid, 0) + 1
                save("official-downloads.json", counts)
                meta = official
            else:
                items = load("mappings.json", [])
                meta = next((m for m in items if m["id"] == mid), None)
                if not meta:
                    return self.error_json("Mapeo inexistente.", HTTPStatus.NOT_FOUND)
                meta["downloads"] = meta.get("downloads", 0) + 1
                save("mappings.json", items)
                path = os.path.join(UPLOADS, mid + ".json")
        try:
            with open(path, "rb") as f:
                data = f.read()
        except FileNotFoundError:
            return self.error_json("Archivo no encontrado.", HTTPStatus.NOT_FOUND)
        slug = re.sub(r"[^a-z0-9]+", "-", f"{meta.get('brand', '')} {meta.get('model', '')}".lower()).strip("-") or "mapeo"
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Disposition", f'attachment; filename="openmix-{slug}.midi.json"')
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def create_mapping(self, body):
        self.throttle(body)
        brand = clean(body.get("brand"), 40)
        model = clean(body.get("model"), 60)
        author = clean(body.get("author"), 40)
        description = clean(body.get("description"), 1500, required=False, multiline=True)
        raw = body.get("mapping")
        if not isinstance(raw, str) or len(raw.encode("utf-8")) > MAX_MAPPING:
            raise ValueError("Adjuntá un archivo midi.json de hasta 512 KB.")
        try:
            doc = json.loads(raw)
        except json.JSONDecodeError:
            raise ValueError("El archivo no es JSON válido.")
        summary = validate_mapping(doc)
        mid = secrets.token_hex(6)
        entry = {"id": mid, "brand": brand, "model": model, "name": f"{brand} {model}", "author": author,
                 "description": description, "downloads": 0, "created": now(), **summary}
        with lock:
            os.makedirs(UPLOADS, exist_ok=True)
            with open(os.path.join(UPLOADS, mid + ".json"), "w", encoding="utf-8") as f:
                json.dump({"version": doc.get("version", 1), "bindings": doc["bindings"]}, f, ensure_ascii=False, indent=1)
            items = load("mappings.json", [])
            items.append(entry)
            save("mappings.json", items)
        self.send_json({"mapping": public_mapping(dict(entry, official=False))}, HTTPStatus.CREATED)

    def promote_mapping(self, mid):
        """Admin: convierte un mapeo de la comunidad en oficial (lo copia a official/)."""
        if not self.is_admin():
            return self.error_json("No autorizado.", HTTPStatus.FORBIDDEN)
        with lock:
            items = load("mappings.json", [])
            entry = next((m for m in items if m["id"] == mid), None)
            if not entry:
                return self.error_json("Mapeo inexistente.", HTTPStatus.NOT_FOUND)
            src = os.path.join(UPLOADS, mid + ".json")
            file = f"{mid}.midi.json"
            with open(src, encoding="utf-8") as f, open(os.path.join(OFFICIAL, file), "w", encoding="utf-8") as g:
                g.write(f.read())
            index_path = os.path.join(OFFICIAL, "index.json")
            try:
                with open(index_path, encoding="utf-8") as f:
                    index = json.load(f)
            except FileNotFoundError:
                index = {"mappings": []}
            index["mappings"].append({k: entry[k] for k in ("id", "brand", "model", "name", "author", "description",
                                                            "bindings", "actions", "devices", "created")} | {"file": file})
            with open(index_path, "w", encoding="utf-8") as f:
                json.dump(index, f, ensure_ascii=False, indent=1)
            save("mappings.json", [m for m in items if m["id"] != mid])
            os.remove(src)
        self.send_json({"ok": True})

    # ── foro ────────────────────────────────────────────────────────────────
    def categories(self):
        with lock:
            threads = load("forum.json", {"threads": []})["threads"]
        counts = {}
        for t in threads:
            counts[t["category"]] = counts.get(t["category"], 0) + 1
        self.send_json({"categories": [dict(c, threads=counts.get(c["id"], 0)) for c in CATEGORIES]})

    def list_threads(self, q):
        with lock:
            threads = load("forum.json", {"threads": []})["threads"]
        cat = q.get("category")
        if cat:
            threads = [t for t in threads if t["category"] == cat]
        text = q.get("q", "").lower().strip()
        if text:
            threads = [t for t in threads if text in (t["title"] + " " + t["body"]).lower()]
        threads = sorted(threads, key=lambda t: (t.get("pinned", False), t["activity"]), reverse=True)
        self.send_json({"threads": [
            {"id": t["id"], "category": t["category"], "title": t["title"], "author": t["author"],
             "created": t["created"], "activity": t["activity"], "replies": len(t["replies"]),
             "excerpt": t["body"][:180], "pinned": t.get("pinned", False)}
            for t in threads]})

    def get_thread(self, tid):
        with lock:
            threads = load("forum.json", {"threads": []})["threads"]
        t = next((t for t in threads if t["id"] == tid), None)
        if not t:
            return self.error_json("El hilo no existe.", HTTPStatus.NOT_FOUND)
        self.send_json({"thread": t})

    def create_thread(self, body):
        self.throttle(body)
        category = body.get("category")
        if category not in CATEGORY_IDS:
            raise ValueError("Elegí una categoría.")
        if category in ADMIN_ONLY_CATEGORIES and not self.is_admin():
            raise ValueError("Solo el equipo de OpenMix publica en Anuncios.")
        thread = {"id": secrets.token_hex(5), "category": category,
                  "title": clean(body.get("title"), 120), "author": clean(body.get("author"), 40),
                  "body": clean(body.get("body"), 8000, multiline=True),
                  "created": now(), "replies": []}
        thread["activity"] = thread["created"]
        with lock:
            forum = load("forum.json", {"threads": []})
            forum["threads"].append(thread)
            save("forum.json", forum)
        self.send_json({"thread": thread}, HTTPStatus.CREATED)

    def create_reply(self, tid, body):
        self.throttle(body)
        reply = {"id": secrets.token_hex(5), "author": clean(body.get("author"), 40),
                 "body": clean(body.get("body"), 8000, multiline=True), "created": now()}
        with lock:
            forum = load("forum.json", {"threads": []})
            t = next((t for t in forum["threads"] if t["id"] == tid), None)
            if not t:
                return self.error_json("El hilo no existe.", HTTPStatus.NOT_FOUND)
            if len(t["replies"]) >= 1000:
                raise ValueError("El hilo está cerrado.")
            t["replies"].append(reply)
            t["activity"] = reply["created"]
            save("forum.json", forum)
        self.send_json({"reply": reply}, HTTPStatus.CREATED)


def main():
    port = int(os.environ.get("PORT", "8080"))
    host = os.environ.get("HOST", "127.0.0.1")
    os.makedirs(UPLOADS, exist_ok=True)
    httpd = ThreadingHTTPServer((host, port), Handler)
    print(f"OpenMix web → http://{host}:{port}" + ("  (moderación activada)" if ADMIN_TOKEN else ""))
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
