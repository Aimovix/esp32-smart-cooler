#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>

// ==========================================
// PIN-KONFIGURATION (ESP32)
// ==========================================
#define PIN_I2C_SDA         21
#define PIN_I2C_SCL         22
#define PIN_DHT_IN          4    // DHT22 #1: Lufteinlass (Raumluft)
#define PIN_DHT_OUT         16   // DHT22 #2: Luftauslass (Kaltluft nach Flaschen)
#define PIN_US_TRIG         5
#define PIN_US_ECHO         18
#define PIN_FAN_PWM         19   // PWM-Signal für alle 5 kaskadierten Arctic P12
#define PIN_BUTTON          15

#define DHTTYPE             DHT22
#define SCREEN_WIDTH        128
#define SCREEN_HEIGHT       64
#define OLED_RESET          -1
#define OLED_ADDR           0x3C

// PWM-Parameter für Arctic P12 (Intel 4-Pin Spezifikation: 25 kHz)
#define PWM_FREQ            25000
#define PWM_RESOLUTION      8
#define PWM_CHANNEL         0

// Physikalische Konstanten zur Kälteleistungsberechnung
#define MAX_AIRFLOW_M3H     475.0f   // 5x Arctic P12 bei 100% ca. 475 m³/h
#define AIR_DENSITY         1.20f    // kg/m³
#define AIR_SPECIFIC_HEAT   1005.0f  // J/(kg * K)

// ==========================================
// SYSTEM-ZUSTÄNDE & PROFILE
// ==========================================
enum SystemMode {
    MODE_AUTO = 0,
    MODE_MANUAL = 1
};

enum ButtonProfile {
    PROFILE_AUTO = 0,
    PROFILE_WHISPER_50 = 1,
    PROFILE_TURBO_100 = 2,
    PROFILE_OFF = 3
};

// ==========================================
// GLOBALE OBJEKTE & VARIABLEN
// ==========================================
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
DHT dhtIn(PIN_DHT_IN, DHTTYPE);
DHT dhtOut(PIN_DHT_OUT, DHTTYPE);
WebServer server(80);

// Messwerte Einlass (Raumklima)
float tempIn = 27.5;
float humIn = 55.0;

// Messwerte Auslass (Kaltluft)
float tempOut = 24.2;
float humOut = 58.0;

// Berechnete Thermodynamik
float coolingPowerWatts = 0.0;
float deltaT = 0.0;
bool iceDepleted = false;

// Präsenzerkennung
long currentDistanceCm = 50;
bool isPresent = true;

// Lüfter-Zustand
SystemMode currentMode = MODE_AUTO;
ButtonProfile currentProfile = PROFILE_AUTO;
int currentFanPwmPercent = 0;   // 0 - 100%

// Timing
unsigned long lastSensorRead = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastPresentTime = 0;
const unsigned long ABSENCE_TIMEOUT_MS = 120000; // 2 Minuten bis Standby

// WLAN Access Point
const char* AP_SSID = "SmartCooler-MINT";
const char* AP_PASS = "12345678";

// ==========================================
// HARDWARE-TREIBER
// ==========================================
void setFanSpeedPercent(int percent) {
    percent = constrain(percent, 0, 100);
    currentFanPwmPercent = percent;

    // Arctic 0-dB Modus: unter 5% stoppt der Motor komplett
    int dutyCycle = 0;
    if (percent >= 5) {
        dutyCycle = map(percent, 0, 100, 0, 255);
    }

#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
    ledcWrite(PIN_FAN_PWM, dutyCycle);
#else
    ledcWrite(PWM_CHANNEL, dutyCycle);
#endif
}

long readUltrasonicDistance() {
    digitalWrite(PIN_US_TRIG, LOW);
    delayMicroseconds(2);
    digitalWrite(PIN_US_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(PIN_US_TRIG, LOW);

    long duration = pulseIn(PIN_US_ECHO, HIGH, 25000);
    if (duration == 0) return 999;
    return duration * 0.0343 / 2;
}

// ==========================================
// THERMODYNAMIK-BERECHNUNG
// ==========================================
void calculateThermodynamics() {
    deltaT = tempIn - tempOut;

    if (currentFanPwmPercent < 5 || deltaT <= 0.0f) {
        coolingPowerWatts = 0.0f;
    } else {
        // Luftvolumenstrom in m³/h
        float airflowM3h = (currentFanPwmPercent / 100.0f) * MAX_AIRFLOW_M3H;
        // Massenstrom in kg/s
        float massFlowKgS = (airflowM3h / 3600.0f) * AIR_DENSITY;
        // Thermische Leistung: P = m_dot * cp * deltaT (in Watt)
        coolingPowerWatts = massFlowKgS * AIR_SPECIFIC_HEAT * deltaT;
    }

    // Wenn der Lüfter läuft, aber Delta T unter 1.0 K sinkt -> Eis ist geschmolzen!
    if (currentFanPwmPercent >= 30 && deltaT < 1.0f) {
        iceDepleted = true;
    } else if (deltaT >= 1.5f) {
        iceDepleted = false;
    }
}

void evaluateAutoLogic() {
    // 1. Präsenzerkennung
    if (currentDistanceCm <= 80 && currentDistanceCm > 3) {
        lastPresentTime = millis();
        isPresent = true;
    } else {
        if (millis() - lastPresentTime > ABSENCE_TIMEOUT_MS) {
            isPresent = false;
        }
    }

    // 2. Abwesend -> Standby
    if (!isPresent) {
        setFanSpeedPercent(0);
        return;
    }

    // 3. Temperatur-Stufen
    if (tempIn < 23.0f) {
        setFanSpeedPercent(0);
    } else if (tempIn >= 23.0f && tempIn <= 26.0f) {
        setFanSpeedPercent(50); // Leise während Unterricht
    } else {
        // Über 26 °C Raumtemperatur: Erhöhe stufenlos auf 80–100%
        int pwm = map((int)(tempIn * 10), 260, 320, 80, 100);
        setFanSpeedPercent(constrain(pwm, 80, 100));
    }
}

// ==========================================
// OLED DISPLAY RENDERING (128x64)
// ==========================================
void updateOLED() {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    // Zeile 1: Modus & Distanz
    display.setCursor(0, 0);
    display.print(currentMode == MODE_AUTO ? "MODUS: AUTO " : "MODUS: MANUELL ");
    display.setCursor(85, 0);
    display.printf("%ldcm", currentDistanceCm);

    display.drawLine(0, 9, 127, 9, SSD1306_WHITE);

    // Zeile 2: Temperaturen (Einlass vs. Auslass)
    display.setCursor(0, 13);
    display.printf("IN:%.1fC -> OUT:%.1fC", tempIn, tempOut);

    // Zeile 3: Lüfter-Status & Durchsatz
    int approxM3h = (int)((currentFanPwmPercent / 100.0f) * MAX_AIRFLOW_M3H);
    display.setCursor(0, 25);
    display.printf("5x P12: %d%% (%dm3/h)", currentFanPwmPercent, approxM3h);

    // Zeile 4: Echte Kälteleistung
    display.setCursor(0, 37);
    display.printf("LEISTUNG: %.0f WATT", coolingPowerWatts);

    display.drawLine(0, 48, 127, 48, SSD1306_WHITE);

    // Zeile 5: Flaschen-Status & Warnung
    display.setCursor(0, 52);
    if (iceDepleted && currentFanPwmPercent >= 20) {
        display.print("! EIS FLASCHEN WECHSELN !");
    } else if (!isPresent && currentMode == MODE_AUTO) {
        display.print("STANDBY (ABWESEND)");
    } else {
        display.printf("STATUS: OK (DT: -%.1fK)", deltaT);
    }

    display.display();
}

// ==========================================
// WEB-APP HTML / CSS / JAVASCRIPT
// ==========================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Smart Ice-Box AC</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
        body { background: #0b0f19; color: #f1f5f9; padding: 16px; display: flex; justify-content: center; }
        .app-container { width: 100%; max-width: 500px; }
        .header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 20px; }
        .header h1 { font-size: 1.3rem; font-weight: 700; color: #38bdf8; }
        .status-pill { background: #1e293b; padding: 5px 12px; border-radius: 9999px; font-size: 0.75rem; border: 1px solid #22c55e; color: #4ade80; font-weight: 600; }
        .status-pill.warn { border-color: #ef4444; color: #f87171; }

        .mode-toggle { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; background: #1e293b; padding: 4px; border-radius: 12px; margin-bottom: 20px; }
        .mode-btn { padding: 12px; border: none; background: transparent; color: #94a3b8; font-weight: 600; border-radius: 8px; cursor: pointer; transition: 0.2s; font-size: 0.9rem; }
        .mode-btn.active { background: #0284c7; color: #fff; }

        .metrics-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; margin-bottom: 20px; }
        .metric-card { background: #131d2e; border: 1px solid #1e293b; border-radius: 12px; padding: 14px; }
        .metric-card.highlight { border-color: #0284c7; background: #0c1e38; }
        .metric-label { font-size: 0.75rem; color: #94a3b8; text-transform: uppercase; margin-bottom: 4px; }
        .metric-value { font-size: 1.5rem; font-weight: 700; color: #f8fafc; }
        .metric-sub { font-size: 0.75rem; color: #64748b; margin-top: 4px; }

        .oled-preview { background: #000; border: 2px solid #334155; border-radius: 10px; padding: 14px; font-family: monospace; font-size: 0.85rem; line-height: 1.45; color: #38bdf8; margin-bottom: 20px; }
        .oled-line { white-space: pre; }

        .control-section { background: #131d2e; border: 1px solid #1e293b; border-radius: 12px; padding: 16px; margin-bottom: 20px; }
        .control-title { font-size: 0.9rem; font-weight: 600; color: #cbd5e1; margin-bottom: 12px; }
        .slider-group label { display: flex; justify-content: space-between; font-size: 0.85rem; margin-bottom: 8px; color: #cbd5e1; }
        input[type=range] { width: 100%; height: 6px; border-radius: 3px; background: #334155; outline: none; -webkit-appearance: none; }
        input[type=range]::-webkit-slider-thumb { -webkit-appearance: none; width: 22px; height: 22px; border-radius: 50%; background: #38bdf8; cursor: pointer; }

        .preset-grid { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 8px; margin-top: 14px; }
        .preset-btn { padding: 8px; background: #1e293b; border: 1px solid #334155; border-radius: 6px; color: #cbd5e1; font-size: 0.8rem; font-weight: 600; cursor: pointer; }
        .preset-btn:hover { background: #334155; color: #fff; }

        .alert-box { background: #450a0a; border: 1px solid #dc2626; color: #fecaca; padding: 12px; border-radius: 8px; font-size: 0.85rem; margin-bottom: 20px; font-weight: 600; display: none; }
    </style>
</head>
<body>
    <div class="app-container">
        <div class="header">
            <h1>Smart Ice-Box AC</h1>
            <div class="status-pill" id="conn-pill">Live ESP32</div>
        </div>

        <div class="alert-box" id="ice-alert">
            WARNUNG: Flaschen-Eis geschmolzen! Kaltluft-Differenz unter 1,0 K. Bitte Flaschen tauschen!
        </div>

        <div class="mode-toggle">
            <button class="mode-btn active" id="btn-mode-auto" onclick="setMode('auto')">Automatik</button>
            <button class="mode-btn" id="btn-mode-man" onclick="setMode('manual')">Manuell / App</button>
        </div>

        <div class="metrics-grid">
            <div class="metric-card highlight">
                <div class="metric-label">Kälteleistung</div>
                <div class="metric-value" id="val-power">0 W</div>
                <div class="metric-sub">Thermodynamisch live</div>
            </div>
            <div class="metric-card">
                <div class="metric-label">Kaltluft (Auslass)</div>
                <div class="metric-value" id="val-temp-out">--.- °C</div>
                <div class="metric-sub" id="val-delta">Delta T: -- K</div>
            </div>
            <div class="metric-card">
                <div class="metric-label">Raumklima (Einlass)</div>
                <div class="metric-value" id="val-temp-in">--.- °C</div>
                <div class="metric-sub" id="val-hum-in">Feuchte: -- %</div>
            </div>
            <div class="metric-card">
                <div class="metric-label">5x Arctic Lüfter</div>
                <div class="metric-value" id="val-fan">0 %</div>
                <div class="metric-sub" id="val-m3h">0 m³/h Luftstrom</div>
            </div>
        </div>

        <div class="control-section" id="manual-controls" style="opacity: 0.4; pointer-events: none;">
            <div class="control-title">Manuelle Lüftersteuerung</div>
            <div class="slider-group">
                <label><span>Geschwindigkeit (5x P12)</span><span id="slider-val">50%</span></label>
                <input type="range" id="fan-slider" min="0" max="100" value="50" oninput="updateFanSlider(this.value)" onchange="sendManualFan(this.value)">
            </div>
            <div class="preset-grid">
                <button class="preset-btn" onclick="setPreset(0)">Aus (0%)</button>
                <button class="preset-btn" onclick="setPreset(50)">Flüster (50%)</button>
                <button class="preset-btn" onclick="setPreset(100)">Turbo (100%)</button>
            </div>
        </div>

        <div class="control-title" style="margin-bottom: 8px;">OLED Hardware-Monitor (128x64)</div>
        <div class="oled-preview">
            <div class="oled-line" id="oled-1">MODUS: AUTO   DTST: --cm</div>
            <div class="oled-line">--------------------------------</div>
            <div class="oled-line" id="oled-2">IN:--.-C -> OUT:--.-C</div>
            <div class="oled-line" id="oled-3">5x P12: 0% (0m3/h)</div>
            <div class="oled-line" id="oled-4">LEISTUNG: 0 WATT</div>
            <div class="oled-line">--------------------------------</div>
            <div class="oled-line" id="oled-5">INITIALISIERE...</div>
        </div>
    </div>

    <script>
        let currentMode = 'auto';

        function setMode(mode) {
            currentMode = mode;
            document.getElementById('btn-mode-auto').classList.toggle('active', mode === 'auto');
            document.getElementById('btn-mode-man').classList.toggle('active', mode === 'manual');
            
            const manSection = document.getElementById('manual-controls');
            if (mode === 'manual') {
                manSection.style.opacity = '1';
                manSection.style.pointerEvents = 'auto';
            } else {
                manSection.style.opacity = '0.4';
                manSection.style.pointerEvents = 'none';
            }
            fetch('/api/control?mode=' + mode);
        }

        function updateFanSlider(val) {
            document.getElementById('slider-val').innerText = val + '%';
        }

        function sendManualFan(val) {
            if (currentMode !== 'manual') return;
            fetch('/api/control?fan=' + val);
        }

        function setPreset(val) {
            document.getElementById('fan-slider').value = val;
            updateFanSlider(val);
            sendManualFan(val);
        }

        async function fetchStatus() {
            try {
                const res = await fetch('/api/status');
                const data = await res.json();
                
                document.getElementById('val-power').innerText = Math.round(data.power_watt) + ' W';
                document.getElementById('val-temp-out').innerText = data.temp_out.toFixed(1) + ' °C';
                document.getElementById('val-delta').innerText = 'Delta T: -' + (data.temp_in - data.temp_out).toFixed(1) + ' K';
                document.getElementById('val-temp-in').innerText = data.temp_in.toFixed(1) + ' °C';
                document.getElementById('val-hum-in').innerText = 'Feuchte: ' + Math.round(data.hum_in) + ' %';
                document.getElementById('val-fan').innerText = data.fan_pwm + ' %';
                document.getElementById('val-m3h').innerText = Math.round((data.fan_pwm / 100) * 475) + ' m³/h Luftstrom';

                // Eis-Leer-Warnung
                document.getElementById('ice-alert').style.display = data.ice_depleted ? 'block' : 'none';

                // OLED Preview Update
                document.getElementById('oled-1').innerText = (data.mode === 'auto' ? 'MODUS: AUTO   ' : 'MODUS: MANUELL ') + 'DTST: ' + data.distance + 'cm';
                document.getElementById('oled-2').innerText = 'IN:' + data.temp_in.toFixed(1) + 'C -> OUT:' + data.temp_out.toFixed(1) + 'C';
                document.getElementById('oled-3').innerText = '5x P12: ' + data.fan_pwm + '% (' + Math.round((data.fan_pwm / 100) * 475) + 'm3/h)';
                document.getElementById('oled-4').innerText = 'LEISTUNG: ' + Math.round(data.power_watt) + ' WATT';
                document.getElementById('oled-5').innerText = data.ice_depleted ? '! EIS FLASCHEN WECHSELN !' : (data.present ? 'STATUS: OK (KUEHLT)' : 'STANDBY (ABWESEND)');

            } catch (e) {
                document.getElementById('conn-pill').classList.add('warn');
                document.getElementById('conn-pill').innerText = 'Getrennt';
            }
        }

        setInterval(fetchStatus, 1000);
        fetchStatus();
    </script>
</body>
</html>
)rawliteral";

// ==========================================
// REST-API HANDLER
// ==========================================
void handleRoot() {
    server.send_P(200, "text/html", INDEX_HTML);
}

void handleStatusApi() {
    JsonDocument doc;
    doc["temp_in"] = tempIn;
    doc["hum_in"] = humIn;
    doc["temp_out"] = tempOut;
    doc["hum_out"] = humOut;
    doc["power_watt"] = coolingPowerWatts;
    doc["distance"] = currentDistanceCm;
    doc["present"] = isPresent;
    doc["fan_pwm"] = currentFanPwmPercent;
    doc["mode"] = (currentMode == MODE_AUTO) ? "auto" : "manual";
    doc["ice_depleted"] = iceDepleted;

    String jsonResponse;
    serializeJson(doc, jsonResponse);
    server.send(200, "application/json", jsonResponse);
}

void handleControlApi() {
    if (server.hasArg("mode")) {
        String modeArg = server.arg("mode");
        if (modeArg == "auto") currentMode = MODE_AUTO;
        else if (modeArg == "manual") currentMode = MODE_MANUAL;
    }

    if (currentMode == MODE_MANUAL && server.hasArg("fan")) {
        int fanArg = server.arg("fan").toInt();
        setFanSpeedPercent(fanArg);
    }

    handleStatusApi();
}

// ==========================================
// HARDWARE-TASTER (PROFILE DURCHSCHALTEN)
// ==========================================
void handleHardwareButton() {
    static unsigned long lastDebounce = 0;
    static int lastState = HIGH;

    int reading = digitalRead(PIN_BUTTON);
    if (reading == LOW && lastState == HIGH && (millis() - lastDebounce > 250)) {
        lastDebounce = millis();

        currentProfile = (ButtonProfile)((currentProfile + 1) % 4);

        switch (currentProfile) {
            case PROFILE_AUTO:
                currentMode = MODE_AUTO;
                break;
            case PROFILE_WHISPER_50:
                currentMode = MODE_MANUAL;
                setFanSpeedPercent(50);
                break;
            case PROFILE_TURBO_100:
                currentMode = MODE_MANUAL;
                setFanSpeedPercent(100);
                break;
            case PROFILE_OFF:
                currentMode = MODE_MANUAL;
                setFanSpeedPercent(0);
                break;
        }
    }
    lastState = reading;
}

// ==========================================
// SETUP & LOOP
// ==========================================
void setup() {
    Serial.begin(115200);

    pinMode(PIN_US_TRIG, OUTPUT);
    pinMode(PIN_US_ECHO, INPUT);
    pinMode(PIN_BUTTON, INPUT_PULLUP);

    // 25 kHz PWM für 5x Arctic P12 Lüfter einrichten
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
    ledcAttach(PIN_FAN_PWM, PWM_FREQ, PWM_RESOLUTION);
#else
    ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RESOLUTION);
    ledcAttachPin(PIN_FAN_PWM, PWM_CHANNEL);
#endif
    setFanSpeedPercent(0);

    // I2C & OLED Display
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println("OLED nicht gefunden!");
    }
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(10, 25);
    display.print("ICE-BOX AC START...");
    display.display();

    // 2x DHT22 Sensoren starten
    dhtIn.begin();
    dhtOut.begin();

    // WLAN Access-Point starten
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.print("Access Point aktiv. IP: ");
    Serial.println(WiFi.softAPIP());

    // Webserver Routen
    server.on("/", handleRoot);
    server.on("/api/status", handleStatusApi);
    server.on("/api/control", handleControlApi);
    server.begin();

    lastPresentTime = millis();
}

void loop() {
    server.handleClient();
    handleHardwareButton();

    unsigned long now = millis();

    // Sensoren alle 1000ms abfragen
    if (now - lastSensorRead >= 1000) {
        lastSensorRead = now;

        float tin = dhtIn.readTemperature();
        float hin = dhtIn.readHumidity();
        float tout = dhtOut.readTemperature();
        float hout = dhtOut.readHumidity();

        if (!isnan(tin)) tempIn = tin;
        if (!isnan(hin)) humIn = hin;
        if (!isnan(tout)) tempOut = tout;
        if (!isnan(hout)) humOut = hout;

        long dist = readUltrasonicDistance();
        if (dist > 0 && dist < 400) {
            currentDistanceCm = dist;
        }

        // Thermodynamische Kälteleistung berechnen
        calculateThermodynamics();

        // Automatik-Logik auswerten
        if (currentMode == MODE_AUTO) {
            evaluateAutoLogic();
        }
    }

    // Display alle 500ms aktualisieren
    if (now - lastDisplayUpdate >= 500) {
        lastDisplayUpdate = now;
        updateOLED();
    }
}
