# esp32.webradio

![Version](https://img.shields.io/badge/version-2.0.0-blue)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Donate](https://img.shields.io/badge/Donate-PayPal-00457C.svg?logo=paypal)](https://www.paypal.com/donate/?business=martin%40bchmnn.de&currency_code=EUR)

> **Web-Radio für ESP32** — Internet-Radio direkt im Browser, basierend auf [esp-hub-base](https://github.com/MPunktBPunkt/iobroker.esp-hub). Kein Audio-Shield nötig — der Browser spielt ab.

---

## Features v2.0.0

- 🎵 **Web-Radio im Browser** — HTML5 Audio, keine extra Hardware
- ⭐ **Favoriten** — reboot-fest auf ESP gespeichert, per ★ aus der Suche hinzufügen
- 🔍 **Entdecken-Tab** — Sendersuche + 21 Genre-Chips via [radio-browser.info](https://www.radio-browser.info)
- 🎙 **Podcasts-Tab** — RSS-Feed laden, Episodenliste, direkt abspielen
- 🕒 **Letzter Sender** — ESP-seitig persistent (jedes Gerät/Browser)
- 📻 **7 Sender vorinstalliert** — Rockantenne + Radio BOB!
- 🚀 **OTA-Update** + **ESP-Hub Integration**

---

## Quickstart

### Bibliotheken (Arduino Library Manager)
- **WiFiManager** von tablatronix / tzapu
- **ArduinoJson** von bblanchon (v6 oder v7)

### Konfiguration
```cpp
#define DEVICE_NAME  "Web-Radio"
#define HUB_HOST     "192.168.178.113"   // IP deines ioBroker
#define HUB_PORT     8093
```

### Flashen → WLAN einrichten → öffnen
```
http://<ESP-IP>/
```
> WLAN zurücksetzen: BOOT-Taste 3s beim Einschalten halten

---

## Web-Oberfläche

| Tab | Inhalt |
|---|---|
| 🎵 **Radio** | Now-Playing · Favoriten · Meine Sender · Sender hinzufügen |
| 🔍 **Entdecken** | Freitextsuche · 21 Genre-Chips · ★ direkt zu Favoriten |
| 🎙 **Podcasts** | RSS-Feed → Episodenliste → abspielen · eigene Feeds |
| 📊 **Status** | Chip · IP · RAM · Flash · Hub-Info |
| 🚀 **OTA** | Firmware Drag & Drop |

### Genre-Browser
Rock · Pop · Metal · Jazz · Blues · Klassik · Electronic · House · Techno · Country · Reggae · R&B · Hip-Hop · Soul · Ambient · Folk · Punk · Indie · News · Talk · Deutsch

---

## Vorinstallierte Sender & Podcasts

**Sender:** Rockantenne Bayern, Rockantenne Heimweh, Radio BOB!, Radio BOB! Rock Classics, Radio BOB! Metal, Radio BOB! Blues, Radio BOB! Reggae

**Podcasts:** Lage der Nation · Chaosradio · WDR 5 Neugier genügt · Deutschlandfunk Andruck

---

## API (ESP32, Port 80)

```
GET  /api/stations           Sender-Liste
POST /api/stations-add       {name, url}
POST /api/stations-delete    {index}
GET  /api/favorites          Favoriten-Liste
POST /api/favorites-add      {name, url}
POST /api/favorites-remove   {index}
GET  /api/podcasts           Podcast-Liste
POST /api/podcasts-add       {name, url}
POST /api/podcasts-remove    {index}
GET  /api/last               {name, url} letzter Sender
POST /api/last               Letzten Sender speichern
```

---

## Lizenz

GNU General Public License v3.0 © MPunktBPunkt

[![Donate](https://img.shields.io/badge/Donate-PayPal-00457C.svg?logo=paypal)](https://www.paypal.com/donate/?business=martin%40bchmnn.de&currency_code=EUR)

---

## Changelog

### 2.0.0 (2026-03-23)
- Neu: Favoriten (persistent auf ESP, /api/favorites)
- Neu: Entdecken-Tab mit radio-browser.info API + 21 Genre-Chips
- Neu: Podcasts-Tab mit RSS-Vorschau (allorigins.win CORS-Proxy)
- Neu: Letzter Sender ESP-seitig gespeichert (/api/last)
- Fix: ArduinoJson v6/v7 Kompatibilität

### 1.0.0
- Erstveröffentlichung
