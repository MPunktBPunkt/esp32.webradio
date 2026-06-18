# esp32.webradio

![Version](https://img.shields.io/badge/version-2.3.0-blue)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Donate](https://img.shields.io/badge/Donate-PayPal-00457C.svg?logo=paypal)](https://www.paypal.com/donate/?business=martin%40bchmnn.de&currency_code=EUR)

> **Web-Radio für ESP32** — Internet-Radio direkt im Browser. Kein Audio-Shield nötig. Integriert in [iobroker.esp-hub](https://github.com/MPunktBPunkt/iobroker.esp-hub).

---

## Überblick

Der ESP32 hostet eine Web-Oberfläche mit HTML5-Audio. Sender, Favoriten und Podcasts werden auf dem Gerät gespeichert — der Browser spielt ab. Aktueller Sender und Songtitel erscheinen im ESP-Hub-Dashboard. Basiert auf [esp-hub-base](https://github.com/MPunktBPunkt/ESP32.esp-hub).

---

## Features

- **Radio im Browser** — HTML5 Audio, keine extra Hardware
- **Favoriten & eigene Sender** — persistent auf dem ESP
- **Entdecken-Tab** — Sendersuche + 21 Genre-Chips via [radio-browser.info](https://www.radio-browser.info)
- **Podcasts** — RSS-Feed laden und Episoden abspielen
- **ICY-Metadata** — Songtitel aus dem Stream (HTTP)
- **ESP-Hub** — `nowStation` und `nowTitle` im Heartbeat
- **mDNS** — `http://<gerätename>.local/`
- **OTA** — Hub-Push oder Browser-Upload

---

## Voraussetzungen

| Typ | Details |
|-----|---------|
| **Board** | ESP32 |
| **WiFiManager** | tablatronix / tzapu |
| **ArduinoJson** | bblanchon v6 oder v7 |
| **ioBroker** | [iobroker.esp-hub](https://github.com/MPunktBPunkt/iobroker.esp-hub) |

---

## Quickstart

1. Sketch öffnen, optional anpassen:

```cpp
#define DEVICE_NAME  "Web-Radio"
#define HUB_HOST     "192.168.178.113"
#define HUB_PORT     8093
```

2. Flashen → Hotspot verbinden → WLAN + Hub-IP eingeben
3. `http://<ESP-IP>/` öffnen

> **WLAN zurücksetzen:** BOOT-Taste 3 s beim Einschalten halten

---

## Web-Oberfläche

| Tab | Inhalt |
|-----|--------|
| **Radio** | Now-Playing, Favoriten, eigene Sender |
| **Entdecken** | Suche, Genre-Chips, ★ zu Favoriten |
| **Podcasts** | RSS-Feeds, Episodenliste |
| **Status** | Systeminfo, Hub-Verbindung |
| **OTA** | Firmware-Update |

---

## ioBroker-Integration (ESP-Hub)

Heartbeat liefert u. a. aktuellen Sender und Titel als IO-Werte unter `esp-hub.0.devices.<MAC>.ios`.

Dashboard: `http://<ioBroker-IP>:8093`

---

## API (Auszug)

| Methode | Pfad | Beschreibung |
|---------|------|--------------|
| GET | `/api/stations` | Sender-Liste |
| GET/POST | `/api/favorites` | Favoriten verwalten |
| GET/POST | `/api/nowplaying` | Aktueller Sender/Titel |
| GET | `/api/status` | Systeminfo JSON |

---

## Lizenz

GNU General Public License v3.0 © MPunktBPunkt — siehe [LICENSE](LICENSE)

[![Donate](https://img.shields.io/badge/Donate-PayPal-00457C.svg?logo=paypal)](https://www.paypal.com/donate/?business=martin%40bchmnn.de&currency_code=EUR)
