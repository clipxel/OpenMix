"""Fuente de SideStore/AltStore para OpenMix Mobile: agrega una versión (la más nueva primero).

Uso: sidestore_source.py <source anterior> <source nuevo> --version X --ipa archivo.ipa --notes notas.md --url URL --repo dueño/repo
La misma fuente la lee la app para avisar que hay una versión nueva (docs/MOVIL.md).
"""
import argparse, datetime, json, os

ap = argparse.ArgumentParser()
ap.add_argument("old")
ap.add_argument("new")
ap.add_argument("--version", required=True)
ap.add_argument("--ipa", required=True)
ap.add_argument("--notes", required=True)
ap.add_argument("--url", required=True)
ap.add_argument("--repo", required=True)
a = ap.parse_args()

try:
    old = json.load(open(a.old))
except (OSError, ValueError):
    old = {}
previous = [v for app in old.get("apps", []) for v in app.get("versions", []) if v.get("version") != a.version]

icon = f"https://raw.githubusercontent.com/{a.repo}/main/Resources/AppIcon.png"
version = {
    "version": a.version,
    "date": datetime.date.today().isoformat(),
    "localizedDescription": open(a.notes, encoding="utf-8").read().strip()[:4000],
    "downloadURL": a.url,
    "size": os.path.getsize(a.ipa),
    "minOSVersion": "15.0",
}
source = {
    "name": "OpenMix",
    "identifier": "app.openmix.source",
    "subtitle": "Software de DJ libre",
    "iconURL": icon,
    "website": f"https://github.com/{a.repo}",
    "apps": [{
        "name": "OpenMix",
        "bundleIdentifier": "app.openmix.OpenMix",
        "developerName": "OpenMix",
        "subtitle": "Software de DJ para el celular",
        "localizedDescription": "Dos decks, sync, hot cues, loops, FX y controladoras de DJ por USB o Bluetooth.",
        "iconURL": icon,
        "tintColor": "F25E46",
        "category": "entertainment",
        "versions": [version] + previous,
        "appPermissions": {
            "entitlements": [],
            "privacy": {
                "NSMicrophoneUsageDescription": "Entradas externas de la interfaz de audio.",
                "NSBluetoothAlwaysUsageDescription": "Controladoras MIDI inalámbricas.",
            },
        },
    }],
    "news": [],
}
json.dump(source, open(a.new, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
print(f"source.json: {len(source['apps'][0]['versions'])} versiones, la más nueva {a.version}")
