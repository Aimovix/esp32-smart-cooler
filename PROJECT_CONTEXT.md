# Projekt-Kontext: Smart Classroom Ice-Box AC (ESP32 MINT-Projekt)

Dieses Dokument ist die zentrale Wissensbasis (Single Source of Truth) für Entwickler und KIs.

---

## 1. Systemübersicht & Physik
Eine mobile 12V-Klimabox (Ice-Box Heat Exchanger) für den Klassenraum, gesteuert durch ein ESP32 NodeMCU Board.
* **Kühlprinzip:** 5 kaskadierte Arctic P12 PWM PST Lüfter pressen bis zu 475 m³/h warme Raumluft durch eine isolierte Thermobox (40–60 Liter), in der 6 bis 8 gefrorene 1,5L-PET-Flaschen als Wärmetauscher stehen. Die Luft kühlt sich an den Flaschenoberflächen trocken ab und tritt als Kaltluftstrom aus.
* **Keine Luftbefeuchtung:** Anders als bei Verdunstungskühlern steigt die Luftfeuchtigkeit im Klassenraum nicht an. Feuchtigkeit aus der Raumluft kondensiert an den kalten Flaschenoberflächen und wird in der dichten Wanne der Box gesammelt.
* **Echte Kälteleistung (ca. 250–350 Watt):** Ausreichend für 4 bis 6 Schulstunden spürbare Kaltluft-Versorgung.
* **Live-Thermodynamik:** Zwei DHT22-Sensoren erfassen $T_{\text{Raum}}$ (Einlass) und $T_{\text{Kalt}}$ (Auslass). Der ESP32 berechnet live die Kälteleistung in Watt und erkennt automatisch, wenn das Eis geschmolzen ist.

---

## 2. Hardware-Architektur & Stromschienen

| Schiene | Quelle | Verbraucher / Last |
| :--- | :--- | :--- |
| **12V DC** | 12V / 3A Steckernetzteil (36 W) | 5× Arctic P12 PWM PST Lüfter (ca. 0,6–0,8 A Gesamtstrom) + LM2596 Eingang |
| **5V DC** | LM2596 Ausgang (präzise auf 5,0V eingestellt) | ESP32 (`VIN`-Pin) |
| **3,3V DC** | ESP32 (`3V3`-Pin) | 0.96" I2C OLED (SSD1306), 2× DHT22, HC-SR04P |
| **GND** | **Common Ground** | 12V-Netzteil, LM2596, ESP32 GND und Lüfter-Masse zwingend verbunden |

---

## 3. Pinbelegung (ESP32 NodeMCU)

| Signal / Funktion | Bauteil-Pin | ESP32 GPIO | Hinweis / Pegel |
| :--- | :--- | :--- | :--- |
| **I2C SDA** | OLED SDA | **GPIO 21** | 3,3V Logik (Standard Hardware I2C) |
| **I2C SCL** | OLED SCL | **GPIO 22** | 3,3V Logik (Standard Hardware I2C) |
| **DHT22 #1 Data** | Einlass (Raumklima) | **GPIO 4** | 10kΩ Pull-Up zu 3,3V |
| **DHT22 #2 Data** | Auslass (Kaltluft) | **GPIO 16** | 10kΩ Pull-Up zu 3,3V |
| **HC-SR04P TRIG** | Trigger | **GPIO 5** | 3,3V Impuls (10 µs) |
| **HC-SR04P ECHO** | Echo | **GPIO 18** | **Muss 3,3V-Version sein (HC-SR04P / RCWL-9610)** |
| **Lüfter PWM** | 5× Arctic P12 PST (Blau) | **GPIO 19** | 25 kHz PWM-Signal (0–100 % Duty Cycle) |
| **Hardware-Taster** | Signal gegen GND | **GPIO 15** | Interner Pullup (`INPUT_PULLUP`), entprellt |

*Hinweis zum Arctic PST-System:* Alle 5 Lüfter werden über die integrierten PST-Stecker kaskadiert (Daisy-Chain). Es wird nur **ein einziges PWM-Kabel** an GPIO 19 und **ein einziges Massekabel** zum gemeinsamen GND geführt. Es wird **kein MOSFET** benötigt.

---

## 4. Berechnungsformel Kälteleistung (Thermodynamik)

Der ESP32 berechnet die thermische Kälteleistung $P$ bei jedem Sensorzyklus:

$$P = \dot{m} \cdot c_p \cdot (T_{\text{Einlass}} - T_{\text{Auslass}})$$

* $\dot{V}_{\max} = 5 \times 95\,\text{m}^3\text{/h} = 475\,\text{m}^3\text{/h}$
* $\dot{V} = \frac{\text{pwmPercent}}{100} \cdot 475\,\text{m}^3\text{/h}$
* Luftdichte $\rho \approx 1{,}20\,\text{kg/m}^3$, Spezifische Wärmekapazität $c_p \approx 1005\,\text{J/(kg}\cdot\text{K)}$
* Massenstrom: $\dot{m} = \frac{\dot{V}}{3600} \cdot \rho$
* Wenn $\Delta T = (T_{\text{Einlass}} - T_{\text{Auslass}}) \le 1{,}0\,\text{K}$ bei laufendem Lüfter:  
  **Status `ICE_EMPTY = true` &rarr; Alarm: „Eis geschmolzen – Flaschen wechseln!“**

---

## 5. Betriebsmodi & Profile

### A. Taster-Profile (Zyklus via GPIO 15)
1. **Smart-Auto:** Regelt Lüfterdrehzahl nach Raumtemperatur; stoppt bei Abwesenheit (>80 cm für >2 Min.).
2. **Unterrichts-Flüstermodus:** 50 % PWM (ca. 240 m³/h, extrem leise, <20 dB).
3. **Pausen-Turbo:** 100 % PWM (475 m³/h maximaler Durchsatz zum Schnellabkühlen).
4. **Aus:** 0 % PWM (Lüfter stehen im 0-dB-Modus).

### B. Web-App / REST-API
* ESP32 spannt eigenen Hotspot auf: `SmartCooler-MINT` (192.168.4.1)
* Endpunkte:
  * `GET /api/status`: JSON mit `temp_in`, `hum_in`, `temp_out`, `hum_out`, `power_watt`, `fan_pwm`, `ice_empty`, `mode`.
  * `GET /api/control`: Parameter `mode` (auto/manual), `fan` (0–100).
