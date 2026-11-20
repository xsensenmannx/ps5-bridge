# PS5 Bridge

Homebrew-ELF für **PS4 und PS4 Pro**, das die Spielebibliothek von
[ps5upload](https://github.com/phantomptr/ps5upload) auf der PS5 anzeigt und
Titel direkt startet. Die Bedienung läuft im Browser der PS4 – ohne App, ohne
Streaming.

![Status](https://img.shields.io/badge/platform-PS4%20%7C%20PS4%20Pro-2b6cff)
![Lizenz](https://img.shields.io/badge/license-GPL--3.0-29e39a)

## Funktionen

- Spiele-Grid mit Icons, Suche und PS5/PS4-Filter
- Spiel antippen und auf der PS5 starten
- Laufendes Spiel in den Vordergrund holen
- Bei laufendem anderen Titel: beenden und wechseln
- Status der PS5: Modell, Firmware, ps5upload-Version, Temperatur
- Automatische Suche im lokalen `/24`-Netz, manuelle IP-Eingabe als Fallback
- Einstellungen bleiben unter `/data/ps5bridge/config.ini` erhalten

Nicht enthalten: Streaming/„Play“, Spiel-Konvertierung und Gäste-Einladungen.
Für Bild und Ton auf der PS4 den offiziellen Weg **PS5 Remote Play** nutzen
(die Weboberfläche erklärt die Schritte). Für PKG-Installation, Dateien, SMB
und mehr verlinkt die Oberfläche direkt auf die PS5-Minion-Seite.

## Voraussetzungen

- PS4 oder PS4 Pro mit einem ELF-Loader (z. B. GoldHEN, Port 9090)
- PS5 mit laufendem **ps5upload**-Payload und **aktiviertem LAN-Zugriff**
- PS4 und PS5 im selben Netzwerk

Die Bridge verbindet sich mit der Minion-Web-API auf Port **9180**. Sie öffnet
selbst Port **9181** für die Weboberfläche.

## Installation

1. `ps5-bridge.elf` aus dem [Release](../../releases) herunterladen.
2. ELF per GoldHEN-Payload-Loader senden:

   ```text
   http://<PS4-IP>:9090/   ->  ps5-bridge.elf auswählen
   ```

   Alternativ die ELF per `elfldr` oder von USB starten.
3. Im Browser der PS4 öffnen:

   ```text
   http://<PS4-IP>:9181/
   ```

4. **Suchen** antippen oder die IP der PS5 eintragen und speichern.

Beim ersten Start ist noch keine PS5 hinterlegt. Nach dem Speichern verbindet
sich die Bridge automatisch und lädt die Bibliothek.

## Fehlerbilder

| Anzeige | Ursache |
| --- | --- |
| `LAN-Zugriff in ps5upload aktivieren` | ps5upload lauscht nur auf Loopback. LAN-Zugriff in ps5upload einschalten. |
| `unreachable` | PS5 aus, ps5upload nicht gestartet oder falsche IP/Port. |
| `starting` | ps5upload startet gerade; kurz warten und aktualisieren. |
| Keine Icons | Icon-Datei fehlt auf der PS5 oder `fs/read` ist deaktiviert. |

## API der Bridge

| Methode | Pfad | Zweck |
| --- | --- | --- |
| `GET` | `/api/state` | Bridge- und PS5-Status |
| `GET` | `/api/games` | Spieleliste (PS5 + PS4) |
| `GET` | `/api/icon?title_id=…` | Icon als PNG/JPEG |
| `POST` | `/api/launch` | `{"title_id":"PPSA00000"}` |
| `POST` | `/api/close` | `{"app_id":123}` |
| `POST` | `/api/config` | `{"ip":"192.168.1.50","port":9180}` |
| `POST` | `/api/discover` | Sucht Konsolen im lokalen Netz |

Die Weboberfläche wird direkt aus der ELF ausgeliefert; es gibt keine externen
Dateien oder CDN-Abhängigkeiten.

## Selbst bauen

Voraussetzungen: Linux (oder WSL), `clang`, `lld`, `make`, `python3` und das
[ps4-payload-sdk](https://github.com/ps4-payload-dev/sdk) v0.9.

```bash
git clone https://github.com/xsensenmannx/ps5-bridge.git
cd ps5-bridge
export PS4_PAYLOAD_SDK=/opt/ps4-payload-sdk
make
```

Ergebnis: `ps5-bridge.elf`.

Ein eigenes Icon kann als `web/icon.png` abgelegt werden. Der Build bettet es
automatisch als Favicon ein; ohne Datei bleibt die Oberfläche bei einem
schlichten Standard-Icon.

## Projektstruktur

```text
src/            C-Quellen des Payloads
  http_server.c kleiner HTTP-Server auf Port 9181
  ps5.c         Client für die ps5upload-Minion-API
  discover.c    /24-Suche nach Port 9180
  json.c        JSON-Parser (jsmn)
  config.c      Einstellungen in /data/ps5bridge
web/index.html  komplette Weboberfläche (eine Datei)
tools/          Build-Helfer (HTML -> C-Header)
third_party/    jsmn (MIT)
```

## Credits

- [ps5upload](https://github.com/phantomptr/ps5upload) – Minion-API und Spielebibliothek
- [ps5drive](https://github.com/phantomptr/ps5drive) – PS4-Payload-Aufbau und State-Pfade
- [ps4-payload-dev/sdk](https://github.com/ps4-payload-dev/sdk) – Toolchain
- [jsmn](https://github.com/zserge/jsmn) – JSON-Parser (MIT)

## Lizenz

GPL-3.0. Siehe [LICENSE](LICENSE).
