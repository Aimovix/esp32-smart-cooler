# ESP32 Smart Desk-Cooler (MINT-Projekt)

Ein intelligenter persönlicher Schreibtisch-Verdunstungskühler (Personal Spot-Cooler), gesteuert durch ein ESP32 NodeMCU Board mit integrierter Web-App, OLED-Statusanzeige und adaptiver Sensor-Regelung.

---

## Features

* **Automatische Temperatur- & Feuchteregelung:** Passt die Lüfterdrehzahl stufenlos an und schaltet den Kaltwasser-Nebel bei Bedarf zu.
* **Tropenhaus-Sperre:** Schaltet den Ultraschall-Zerstäuber ab einer relativen Luftfeuchtigkeit von >65 % automatisch ab, um Schwüle und Kondenswasser auf dem Schreibtisch zu verhindern (Hysterese reaktiviert bei $\le$60 %).
* **Ultraschall-Präsenzerkennung (Radar):** Geht in den stromsparenden Standby-Modus (0 RPM, Nebler aus), wenn der Nutzer länger als 2 Minuten abwesend ist (>80 cm Abstand).
* **Dual-Steuerung (Web-App & Hardware-Taster):**
  * Eigener WLAN-Hotspot (`SmartCooler-MINT`) liefert eine responsive Dark-Mode Web-App aus (völlig unabhängig von externem WLAN).
  * Hardware-Taster am Gehäuse zum Durchschalten von 4 Profilen (*Smart-Auto* &rarr; *Lüfter 50 %* &rarr; *Max Boost 100 %* &rarr; *Aus*).
* **0.96" I2C OLED Display:** Zeigt Live-Messwerte, Drehzahl, Modus und Betriebszustand direkt am Gerät an.

---

## Hardware & Verdrahtung

| Komponente | Signal / Pin | ESP32 GPIO | Betriebsspannung |
| :--- | :--- | :--- | :--- |
| **0.96" I2C OLED** | SDA / SCL | **GPIO 21 / GPIO 22** | 3,3V (ESP32 3V3) |
| **DHT22 Sensor** | DATA | **GPIO 4** | 3,3V (ESP32 3V3) |
| **HC-SR04P Sensor** | TRIG / ECHO | **GPIO 5 / GPIO 18** | 3,3V (ESP32 3V3) |
| **Arctic P12 PWM Lüfter** | PWM (Pin 4, Blau) | **GPIO 19** | 12V (Netzteil direkt) |
| **D4184 / LR7843 MOSFET** | Gate | **GPIO 23** | 3,3V Steuersignal |
| **5V-Piezo-Zerstäuber** | VCC / GND | Über MOSFET geschaltet | 5,0V (LM2596 Ausgang) |
| **Hardware-Taster** | Signal gegen GND | **GPIO 15** | Interner Pullup (`INPUT_PULLUP`) |

> [!IMPORTANT]
> Alle Masseleitungen (12V-Netzteil, LM2596 Ein-/Ausgang, ESP32 GND, MOSFET GND und Sensoren) müssen zwingend als **Common Ground** miteinander verbunden sein.

---

## Projektstruktur

```
├── .gitignore
├── platformio.ini       # PlatformIO Projekt-Konfiguration
├── PROJECT_CONTEXT.md   # Detaillierte technische Spezifikation für KIs
├── README.md            # Projektübersicht
├── simulator.html       # Lokaler interaktiver Simulator (im Browser ausführbar)
└── src/
    └── main.cpp         # Vollständige Firmware mit Web-App & REST-API
```

---

## Verwendung

### 1. Vorab-Simulation im Browser
Die Datei [`simulator.html`](./simulator.html) kann ohne Programmieraufwand direkt in jedem Webbrowser geöffnet werden, um die Steuerungslogik mit virtuellen Reglern interaktiv zu testen.

### 2. Flashen auf den ESP32
Mit [PlatformIO](https://platformio.org/) in VS Code / Antigravity öffnen und den Upload starten:
```bash
pio run -t upload
```
Nach dem Start spannt der ESP32 den Access Point **`SmartCooler-MINT`** (Passwort: `12345678`) auf. Im Browser unter `http://192.168.4.1` öffnet sich das Steuerungs-Dashboard.
