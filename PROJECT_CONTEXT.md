# Projekt-Kontext: Smart Desk-Cooler (ESP32 MINT-Projekt)

Dieses Dokument dient als zentrale Wissensbasis (Single Source of Truth) für Entwickler und KIs.

---

## 1. Systemübersicht & Physik
Ein kompakter Schreibtisch-Verdunstungskühler (Personal Spot-Cooler), gesteuert durch einen ESP32 NodeMCU.
* **Kühlprinzip:** Ein 120-mm-PC-Lüfter saugt Luft an und bläst sie an einer Ultraschall-Piezoscheibe vorbei. Das zerstäubte Wasser verdunstet im Luftstrom und kühlt die Haut (Verdampfungsenthalpie + Windchill).
* **Sicherheits- & Sparfunktionen:** 
  * Ultraschall-Präsenzerkennung: Schaltet in den Standby, wenn niemand am Schreibtisch sitzt (> 80 cm).
  * Tropenhaus-Sperre: Schaltet den Nebler ab > 65 % relativer Luftfeuchtigkeit ab, um Schwüle und Kondenswasser zu verhindern.
* **Bedienung:** Dual – über Hardware-Taster am Gehäuse und über WLAN (Web-App / REST-API).

---

## 2. Hardware-Architektur & Stromschienen

| Schiene | Quelle | Verbraucher / Last |
| :--- | :--- | :--- |
| **12V DC** | 12V / 2A Netzteil | Arctic P12 PWM (VCC & GND) + LM2596 Step-Down Eingang |
| **5V DC** | LM2596 Ausgang (auf 5,0V kalibriert) | ESP32 (`VIN`), Piezo-Zerstäuberplatine (über MOSFET) |
| **3,3V DC** | ESP32 (`3V3` Pin) | 0.96" I2C OLED (SSD1306), DHT22, HC-SR04P |
| **GND** | **Common Ground** | Alle GND-Anschlüsse (12V, 5V, 3,3V, ESP32, Lüfter, MOSFET) zwingend verbunden |

---

## 3. Pinbelegung (ESP32 NodeMCU)

| Signal / Funktion | Bauteil-Pin | ESP32 GPIO | Hinweis / Pegel |
| :--- | :--- | :--- | :--- |
| **I2C SDA** | OLED SDA | **GPIO 21** | 3,3V Logik (Standard Hardware I2C) |
| **I2C SCL** | OLED SCL | **GPIO 22** | 3,3V Logik (Standard Hardware I2C) |
| **DHT22 Data** | Sensor DATA | **GPIO 4** | 10kΩ Pull-Up zu 3,3V |
| **HC-SR04P TRIG** | Trigger | **GPIO 5** | 3,3V Ausgangsimpuls (10 µs) |
| **HC-SR04P ECHO** | Echo | **GPIO 18** | **Muss 3,3V sein** (HC-SR04P / RCWL-9610) |
| **Lüfter PWM** | Arctic P12 Pin 4 (Blau) | **GPIO 19** | 25 kHz PWM-Frequenz (0–100 % Duty Cycle) |
| **Nebler Gate** | D4184 / LR7843 Gate | **GPIO 23** | 3,3V Digital Output (HIGH = Ein, LOW = Aus) |
| **Hardware-Taster** | Taster gegen GND | **GPIO 15** | Interner Pull-Up (`INPUT_PULLUP`), entprellt |

---

## 4. Hardware-Constraints (Nicht verändern)
1. **MOSFET-Wahl:** Ausschließlich 3,3V-Logic-Level-MOSFETs (D4184, LR7843, AOD4184). Niemals IRF520 (schaltet erst ab 5–10V).
2. **Lüfter-Steuerung:** Arctic P12 PWM besitzt einen 0-dB-Modus. Bei einem PWM-Signal < 5 % schaltet der Motor intern komplett ab. Es wird kein separater MOSFET für den Lüfter benötigt.
3. **Zerstäuber-Taster:** Günstige 5V-Piezo-Boards haben einen Standby-Taster. Dieser muss auf der Platine gebrückt sein, damit das Modul sofort nebelt, wenn der MOSFET 5V durchschaltet.
4. **Feuchtigkeitsschutz:** Die Piezo-Platte sitzt außen **vor** dem Auslassgitter des Lüfters. Der Nebel darf keinesfalls ins Gehäuse gesaugt werden.

---

## 5. Steuerlogik & State Machine

### A. Betriebsmodi (`systemMode`)

#### 1. `MODE_AUTO` (Automatik)
* **Präsenzerkennung:**
  * Abstand $\le 80\text{ cm}$: Benutzer anwesend.
  * Abstand $> 80\text{ cm}$ für mehr als 2 Minuten: Umschaltung in `STANDBY` (Lüfter 0 %, Nebler AUS).
* **Temperatur-Kurve (wenn anwesend):**
  * $T < 22^\circ\text{C}$: Lüfter 0 %, Nebler AUS.
  * $22^\circ\text{C} \le T \le 25^\circ\text{C}$: Lüfter 50 % PWM, Nebler AN (sofern $rF \le 65\%$).
  * $T > 25^\circ\text{C}$: Lüfter 80–100 % PWM, Nebler AN (sofern $rF \le 65\%$).
* **Tropenhaus-Sperre:**
  * $rF > 65\%$: Nebler zwingend AUS. Lüfter läuft weiter zur Luftzirkulation.
  * Hysterese: Nebler reaktiviert erst wieder bei $rF \le 60\%$.

#### 2. `MODE_MANUAL` (App / Direkteingabe)
* Manuelle Sollwerte für Lüfter-PWM (0–100 %) und Nebler (AN / AUS).
* Tropenhaus-Sperre bleibt als Sicherheitswächter aktiv (oder warnt).

#### 3. Taster-Profile (Zyklus bei Tastendruck am Gehäuse)
`1. Smart-Auto` $\rightarrow$ `2. Nur Lüfter (50 %)` $\rightarrow$ `3. Max Boost (100 % + Nebel)` $\rightarrow$ `4. Aus`

---

## 6. Software-Architektur (ESP32)

* **Framework:** Arduino / PlatformIO
* **Keine blockierenden Aufrufe:** Kein `delay()`. Alle Tasks (Sensorabfrage, Display-Aktualisierung, PWM, Webserver) laufen über `millis()`-Timer.
* **WLAN & Webserver:**
  * ESP32 startet als Access Point (`SmartCooler-MINT`, IP `192.168.4.1`) und verbindet sich optional mit dem Heim-WLAN.
  * REST-Endpunkte:
    * `GET /api/status`: Liefert JSON (`temp`, `humidity`, `distance`, `fan_pwm`, `mist_state`, `mode`, `presence`).
    * `POST /api/control`: Setzt Parameter (`mode`, `fan_pwm`, `mist_state`).
  * Web-Dashboard: Im Flash-Speicher des ESP32 hinterlegt (Responsive HTML/CSS/JS).
