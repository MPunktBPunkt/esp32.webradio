# esp32.webradio

![Version](https://img.shields.io/badge/version-2.4.0-blue)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Donate](https://img.shields.io/badge/Donate-PayPal-00457C.svg?logo=paypal)](https://www.paypal.com/donate/?business=martin%40bchmnn.de&currency_code=EUR)

> **Web-Radio für ESP32** — Internet-Radio im Browser. Kein Audio-Shield nötig. Integriert in [iobroker.esp-hub](https://github.com/MPunktBPunkt/iobroker.esp-hub).

---

## Überblick

Der ESP hostet die Oberfläche; der Browser spielt Streams ab (HTML5 Audio). Favoriten, Sender und Podcasts liegen im NVS. Entdecken nutzt [radio-browser.info](https://www.radio-browser.info) (Lokal DE, Genres, Quellen DE/DACH/Welt/Community).

**Bluetooth:** Audio kommt aus dem Browser-Tab — BT-Lautsprecher am Handy/PC verbinden. Der ESP streamt nicht per A2DP (ESP32-S3: kein Classic-BT; ESP32: WiFi+BT parallel ungeeignet).

---

## Features (2.4.0)

- Hero Now-Playing UI, Media Session, Sender-Favicons
- Lokal & ÖRR (Bundesländer / Top DE)
- Genre-Chips + Suche; Quellen DE / DACH / Welt / Community
- Podcasts via ESP-RSS-Proxy (`/api/proxy`)
- `/api/status`, `/api/nowplaying`; Hub `nowStation` / `nowTitle`
- OTA, mDNS, ESP-Hub Heartbeat

---

## Quickstart

```cpp
#define DEVICE_NAME  "Web-Radio"
#define HUB_HOST     "192.168.178.113"
#define HUB_PORT     8093
```

Flashen → Hotspot `ESP-WebRadio` → WLAN + Hub → `http://<ESP-IP>/`

> BOOT 3 s = WLAN zurücksetzen

---

## Vorkompilierte Firmware

| Datei | Board |
|-------|-------|
| `webradio.2.4.0.esp32.bin` | ESP32 / D1 Mini |
| `webradio.2.4.0.esp32s3.bin` | ESP32-S3 |

---

## API (Auszug)

| Methode | Pfad | Beschreibung |
|---------|------|--------------|
| GET | `/api/stations` | Sender |
| GET/POST | `/api/favorites*` | Favoriten |
| GET/POST | `/api/last` | Zuletzt gehört |
| GET/POST | `/api/nowplaying` | Aktueller Sender/Titel |
| GET | `/api/status` | Systeminfo |
| GET | `/api/proxy?url=` | RSS-Proxy |

---

## Lizenz

GNU General Public License v3.0 © MPunktBPunkt — siehe [LICENSE](LICENSE)
