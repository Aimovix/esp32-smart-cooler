# ESP32 Smart Ice-Box AC (MINT-Klassenraum-Edition)

Eine mobile 12V-Klimabox (Wärmetauscher) für den Klassenraum, gesteuert durch ein ESP32 NodeMCU Board mit 5 kaskadierten Arctic P12 PWM PST Lüftern, doppelter DHT22-Temperaturmessung, Live-Kälteleistungsberechnung in Watt und integrierter Web-App.

---

## Kühlprinzip & Thermodynamik

* **Trockene Kälteleistung (ca. 250–350 Watt):** 5 Lüfter (bis zu 475 m³/h Durchsatz) pressen warme Raumluft durch eine isolierte Thermobox (40–60 L) mit 6 bis 8 tiefgefrorenen 1,5L-PET-Flaschen (mit Salz-Zusatz).
* **Kein Feuchte-Anstieg (Kein Sauna-Effekt):** Die Raumluft wird nicht befeuchtet. Stattdessen kondensiert schwüle Raumfeuchte an den kalten Flaschenoberflächen und wird in der Wanne gesammelt.
* **Live-Thermodynamik (MINT-Highlight):** Zwei DHT22-Sensoren erfassen $T_{\text{Einlass}}$ und $T_{\text{Auslass}}$. Der ESP32 berechnet in Echtzeit die Kälteleistung in Watt:
  $$P = \dot{m} \cdot c_p \cdot (T_{\text{Einlass}} - T_{\text{Auslass}})$$
* **Automatische Eis-Leer-Erkennung:** Fällt die Temperaturdifferenz $\Delta T$ bei laufendem Lüfter unter 1,0 K, warnt das OLED-Display und die Web-App: *„Eis geschmolzen – Flaschen wechseln!“*.

---

## Hardware & Verdrahtung

| Komponente | Signal / Pin | ESP32 GPIO | Betriebsspannung |
| :--- | :--- | :--- | :--- |
| **0.96" I2C OLED** | SDA / SCL | **GPIO 21 / GPIO 22** | 3,3V (ESP32 3V3) |
| **DHT22 #1 (Einlass)** | DATA | **GPIO 4** | 3,3V (ESP32 3V3) |
| **DHT22 #2 (Auslass)** | DATA | **GPIO 16** | 3,3V (ESP32 3V3) |
| **HC-SR04P (Radar)** | TRIG / ECHO | **GPIO 5 / GPIO 18** | 3,3V (ESP32 3V3) |
| **5× Arctic P12 PWM PST** | PWM (Blaues Kabel) | **GPIO 19** | 12V (Netzteil direkt) |
| **LM2596 Step-Down** | IN / OUT | Eingang 12V &rarr; Ausgang 5,0V | Speist ESP32 `VIN` |
| **Hardware-Taster** | Signal gegen GND | **GPIO 15** | Interner Pullup (`INPUT_PULLUP`) |

> [!NOTE]
> **Kein MOSFET erforderlich:** Durch die Arctic PST-Technologie werden alle 5 Lüfter hintereinander zusammengesteckt. Ein einziges PWM-Kabel steuert alle 5 Lüfter stufenlos direkt vom ESP32 aus.

---

## Projektstruktur

```
├── .gitignore
├── platformio.ini       # PlatformIO Projekt-Konfiguration
├── PROJECT_CONTEXT.md   # Detaillierte Spezifikation als Single Source of Truth
├── README.md            # Projektübersicht & Physik
├── simulator.html       # Lokaler interaktiver Simulator (im Browser ausführbar)
└── src/
    └── main.cpp         # Vollständige Firmware mit Dual-DHT22, Watt-Berechnung & Web-App
```

---

## Verwendung

### 1. Vorab-Simulation im Browser
Öffne [`simulator.html`](./simulator.html) direkt mit einem Doppelklick in deinem Browser, um den Leistungs- und Laufzeitverlauf über 6 Schulstunden interaktiv zu simulieren.

### 2. Flashen auf den ESP32
Mit PlatformIO:
```bash
pio run -t upload
```
Der ESP32 spannt den Hotspot **`SmartCooler-MINT`** (Passwort: `12345678`) auf. Im Browser unter `http://192.168.4.1` öffnet sich das Steuerungs-Dashboard.
