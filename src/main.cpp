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
#define PIN_I2C_SDA     21
#define PIN_I2C_SCL     22
#define PIN_DHT         4
#define PIN_US_TRIG     5
#define PIN_US_ECHO     18
#define PIN_FAN_PWM     19
#define PIN_MIST_GATE   23
#define PIN_BUTTON      15

#define DHTTYPE         DHT22
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1
#define OLED_ADDR       0x3C

// PWM-Parameter für Arctic P12 (Intel 4-Pin PWM Spezifikation: 25 kHz)
#define PWM_FREQ        25000
#define PWM_RESOLUTION  8       // 0 - 255
#define PWM_CHANNEL     0

// ==========================================
// SYSTEM-ZUSTÄNDE & MODI
// ==========================================
enum SystemMode {
    MODE_AUTO = 0,
    MODE_MANUAL = 1
};

enum ButtonProfile {
    PROFILE_AUTO = 0,
    PROFILE_FAN_50 = 1,
    PROFILE_BOOST = 2,
    PROFILE_OFF = 3
};

// ==========================================
// GLOBALE OBJEKTE & VARIABLEN
// ==========================================
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
DHT dht(PIN_DHT, DHTTYPE);
WebServer server(80);

// Sensorwerte
float currentTemp = 24.0;
float currentHumidity = 50.0;
long currentDistanceCm = 50;
bool isPresent = true;

// Aktor-Zustände
SystemMode currentMode = MODE_AUTO;
ButtonProfile currentProfile = PROFILE_AUTO;
int currentFanPwmPercent = 0;   // 0 - 100%
bool currentMistActive = false;
bool tropenhausLockout = false;

// Timing (Non-blocking)
unsigned long lastSensorRead = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastPresentTime = 0;
const unsigned long ABSENCE_TIMEOUT_MS = 120000; // 2 Minuten bis Standby

// WLAN Access-Point Einstellungen
const char* AP_SSID = "SmartCooler-MINT";
const char* AP_PASS = "12345678"; // Min. 8 Zeichen

// ==========================================
// HARDWARE-TREIBER
// ==========================================
void setFanSpeedPercent(int percent) {
    percent = constrain(percent, 0, 100);
    currentFanPwmPercent = percent;
    
    // Arctic P12 0-dB Modus: unter 5% stoppt der Lüfter komplett
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

void setMistActive(bool active) {
    if (tropenhausLockout && active) {
        // Tropenhaus-Sperre verhindert Einschalten
        digitalWrite(PIN_MIST_GATE, LOW);
        currentMistActive = false;
        return;
    }
    currentMistActive = active;
    digitalWrite(PIN_MIST_GATE, active ? HIGH : LOW);
}

long readUltrasonicDistance() {
    digitalWrite(PIN_US_TRIG, LOW);
    delayMicroseconds(2);
    digitalWrite(PIN_US_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(PIN_US_TRIG, LOW);

    // Timeout nach 25ms (ca. 4 Meter max)
    long duration = pulseIn(PIN_US_ECHO, HIGH, 25000);
    if (duration == 0) {
        return 999; // Kein Echo
    }
    return duration * 0.0343 / 2;
}

// ==========================================
// REGELUNG & STATE MACHINE
// ==========================================
void evaluateAutoLogic() {
    // 1. Tropenhaus-Wächter (Hysterese: Aus bei >65%, Reaktivierung erst bei <=60%)
    if (currentHumidity > 65.0) {
        tropenhausLockout = true;
    } else if (currentHumidity <= 60.0) {
        tropenhausLockout = false;
    }

    // 2. Präsenzerkennung (HC-SR04P)
    if (currentDistanceCm <= 80 && currentDistanceCm > 3) {
        lastPresentTime = millis();
        isPresent = true;
    } else {
        if (millis() - lastPresentTime > ABSENCE_TIMEOUT_MS) {
            isPresent = false;
        }
    }

    // 3. Wenn abwesend -> Energiespar-Standby
    if (!isPresent) {
        setFanSpeedPercent(0);
        setMistActive(false);
        return;
    }

    // 4. Regelung nach Temperatur (wenn anwesend)
    if (currentTemp < 22.0) {
        setFanSpeedPercent(0);
        setMistActive(false);
    } else if (currentTemp >= 22.0 && currentTemp <= 25.0) {
        setFanSpeedPercent(50);
        setMistActive(!tropenhausLockout);
    } else { // > 25.0 °C
        // Dynamisch zwischen 80% und 100%
        int pwm = map((int)(currentTemp * 10), 250, 320, 80, 100);
        pwm = constrain(pwm, 80, 100);
        setFanSpeedPercent(pwm);
        setMistActive(!tropenhausLockout);
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
    if (currentMode == MODE_AUTO) {
        display.print("MODUS: AUTO ");
    } else {
        display.print("MODUS: MANUELL ");
    }
    display.setCursor(75, 0);
    if (isPresent) {
        display.printf("%ldcm [DA]", currentDistanceCm);
    } else {
        display.printf("%ldcm [WFG]", currentDistanceCm);
    }

    display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

    // Zeile 2: DHT22 Messwerte
    display.setCursor(0, 14);
    display.printf("KLIMA: %.1fC | %.0f%% rF", currentTemp, currentHumidity);

    // Zeile 3: Lüfter-Status
    int approxRpm = (currentFanPwmPercent * 1800) / 100;
    display.setCursor(0, 26);
    display.printf("LUEFTER(PWM): %d%%", currentFanPwmPercent);
    display.setCursor(85, 26);
    display.printf("(%d)", approxRpm);

    // Zeile 4: Nebler-Status
    display.setCursor(0, 38);
    display.print("NEBLER(MOSFET): ");
    if (tropenhausLockout) {
        display.print("SPERRE");
    } else {
        display.print(currentMistActive ? "EIN" : "AUS");
    }

    display.drawLine(0, 49, 127, 49, SSD1306_WHITE);

    // Zeile 5: System-Zustandsmeldung
    display.setCursor(0, 53);
    if (!isPresent && currentMode == MODE_AUTO) {
        display.print("STANDBY (ABWESEND)");
    } else if (tropenhausLockout) {
        display.print("TROPEN-SPERRE (>65%)");
    } else if (currentMode == MODE_MANUAL) {
        display.print("APP / MANUELL AKTIV");
    } else {
        display.print("SMART-KUEHLUNG AKTIV");
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
    <title>Smart Desk-Cooler</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
        body { background: #0b0f19; color: #f1f5f9; padding: 16px; display: flex; justify-content: center; }
        .app-container { width: 100%; max-width: 480px; }
        .header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 20px; }
        .header h1 { font-size: 1.25rem; font-weight: 700; color: #38bdf8; }
        .status-pill { background: #1e293b; padding: 4px 10px; border-radius: 9999px; font-size: 0.75rem; border: 1px solid #334155; }
        .status-pill.active { border-color: #22c55e; color: #4ade80; }
        
        .mode-toggle { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; background: #1e293b; padding: 4px; border-radius: 12px; margin-bottom: 20px; }
        .mode-btn { padding: 10px; border: none; background: transparent; color: #94a3b8; font-weight: 600; border-radius: 8px; cursor: pointer; transition: 0.2s; }
        .mode-btn.active { background: #0284c7; color: #fff; }

        .metrics-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; margin-bottom: 20px; }
        .metric-card { background: #131d2e; border: 1px solid #1e293b; border-radius: 12px; padding: 14px; }
        .metric-label { font-size: 0.75rem; color: #94a3b8; text-transform: uppercase; margin-bottom: 6px; }
        .metric-value { font-size: 1.4rem; font-weight: 700; color: #f8fafc; }
        .metric-sub { font-size: 0.75rem; color: #64748b; margin-top: 4px; }

        .oled-preview { background: #000; border: 2px solid #334155; border-radius: 8px; padding: 12px; font-family: monospace; font-size: 0.8rem; line-height: 1.4; color: #38bdf8; margin-bottom: 20px; }
        .oled-line { white-space: pre; }

        .control-section { background: #131d2e; border: 1px solid #1e293b; border-radius: 12px; padding: 16px; margin-bottom: 20px; }
        .control-title { font-size: 0.9rem; font-weight: 600; color: #cbd5e1; margin-bottom: 12px; }
        .slider-group { margin-bottom: 16px; }
        .slider-group label { display: flex; justify-content: space-between; font-size: 0.85rem; margin-bottom: 6px; }
        input[type=range] { width: 100%; height: 6px; border-radius: 3px; background: #334155; outline: none; -webkit-appearance: none; }
        input[type=range]::-webkit-slider-thumb { -webkit-appearance: none; width: 20px; height: 20px; border-radius: 50%; background: #38bdf8; cursor: pointer; }

        .toggle-btn { width: 100%; padding: 12px; border: 1px solid #334155; border-radius: 8px; font-weight: 600; font-size: 0.9rem; background: #1e293b; color: #fff; cursor: pointer; }
        .toggle-btn.on { background: #0284c7; border-color: #38bdf8; }
        .alert-box { background: #451a03; border: 1px solid #b45309; color: #fde68a; padding: 10px; border-radius: 8px; font-size: 0.8rem; margin-top: 12px; display: none; }
    </style>
</head>
<body>
    <div class="app-container">
        <div class="header">
            <h1>Smart Desk-Cooler</h1>
            <div class="status-pill active" id="conn-pill">Live ESP32</div>
        </div>

        <div class="mode-toggle">
            <button class="mode-btn active" id="btn-mode-auto" onclick="setMode('auto')">Automatik</button>
            <button class="mode-btn" id="btn-mode-man" onclick="setMode('manual')">Manuell / App</button>
        </div>

        <div class="metrics-grid">
            <div class="metric-card">
                <div class="metric-label">Temperatur</div>
                <div class="metric-value" id="val-temp">--.- °C</div>
                <div class="metric-sub">DHT22 Raumklima</div>
            </div>
            <div class="metric-card">
                <div class="metric-label">Luftfeuchte</div>
                <div class="metric-value" id="val-hum">-- %</div>
                <div class="metric-sub">Sperre ab 65%</div>
            </div>
            <div class="metric-card">
                <div class="metric-label">Abstand (Radar)</div>
                <div class="metric-value" id="val-dist">-- cm</div>
                <div class="metric-sub" id="val-presence">Prüfe...</div>
            </div>
            <div class="metric-card">
                <div class="metric-label">Lüfter-Drehzahl</div>
                <div class="metric-value" id="val-fan">0 %</div>
                <div class="metric-sub" id="val-rpm">0 RPM (0-dB)</div>
            </div>
        </div>

        <div class="control-section" id="manual-controls" style="opacity: 0.4; pointer-events: none;">
            <div class="control-title">Manuelle Steuerung</div>
            <div class="slider-group">
                <label><span>Lüfter-Leistung</span><span id="slider-val">50%</span></label>
                <input type="range" id="fan-slider" min="0" max="100" value="50" oninput="updateFanSlider(this.value)" onchange="sendManualControl()">
            </div>
            <button class="toggle-btn" id="mist-btn" onclick="toggleMist()">Zerstäuber: AUS</button>
            <div class="alert-box" id="tropen-alert">Tropenhaus-Sperre aktiv (>65% rF)! Nebler blockiert.</div>
        </div>

        <div class="control-title" style="margin-bottom: 8px;">OLED Hardware-Monitor (128x64)</div>
        <div class="oled-preview">
            <div class="oled-line" id="oled-line1">MODUS: AUTO   DTST: --cm</div>
            <div class="oled-line">--------------------------------</div>
            <div class="oled-line" id="oled-line2">KLIMA: --.-C | --% rF</div>
            <div class="oled-line" id="oled-line3">LUEFTER(PWM): 0% (0)</div>
            <div class="oled-line" id="oled-line4">NEBLER(MOSF): AUS</div>
            <div class="oled-line">--------------------------------</div>
            <div class="oled-line" id="oled-line5">INITIALISIERE...</div>
        </div>
    </div>

    <script>
        let currentMode = 'auto';
        let mistActive = false;

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

        function sendManualControl() {
            if (currentMode !== 'manual') return;
            const fanVal = document.getElementById('fan-slider').value;
            fetch('/api/control?fan=' + fanVal);
        }

        function toggleMist() {
            if (currentMode !== 'manual') return;
            mistActive = !mistActive;
            fetch('/api/control?mist=' + (mistActive ? '1' : '0'));
        }

        async function fetchStatus() {
            try {
                const res = await fetch('/api/status');
                const data = await res.json();
                
                document.getElementById('val-temp').innerText = data.temp.toFixed(1) + ' °C';
                document.getElementById('val-hum').innerText = Math.round(data.humidity) + ' %';
                document.getElementById('val-dist').innerText = data.distance + ' cm';
                document.getElementById('val-presence').innerText = data.present ? 'Anwesend (<80cm)' : 'Abwesend (Standby)';
                document.getElementById('val-fan').innerText = data.fan_pwm + ' %';
                document.getElementById('val-rpm').innerText = Math.round((data.fan_pwm * 1800) / 100) + ' RPM';

                // Tropenhaus-Warnung
                document.getElementById('tropen-alert').style.display = data.tropen_lock ? 'block' : 'none';

                // Nebler-Button
                const mistBtn = document.getElementById('mist-btn');
                if (data.mist_active) {
                    mistBtn.innerText = 'Zerstäuber: AN';
                    mistBtn.classList.add('on');
                    mistActive = true;
                } else {
                    mistBtn.innerText = 'Zerstäuber: AUS';
                    mistBtn.classList.remove('on');
                    mistActive = false;
                }

                // OLED Preview Update
                document.getElementById('oled-line1').innerText = (data.mode === 'auto' ? 'MODUS: AUTO   ' : 'MODUS: MANUELL') + 'DTST: ' + data.distance + 'cm';
                document.getElementById('oled-line2').innerText = 'KLIMA: ' + data.temp.toFixed(1) + 'C | ' + Math.round(data.humidity) + '% rF';
                document.getElementById('oled-line3').innerText = 'LUEFTER(PWM): ' + data.fan_pwm + '% (' + Math.round((data.fan_pwm * 1800) / 100) + ')';
                document.getElementById('oled-line4').innerText = 'NEBLER(MOSF): ' + (data.tropen_lock ? 'SPERRE' : (data.mist_active ? 'EIN' : 'AUS'));
                document.getElementById('oled-line5').innerText = !data.present && data.mode === 'auto' ? 'STANDBY (ABWESEND)' : (data.tropen_lock ? 'TROPEN-SPERRE (>65%)' : 'STATUS: NORMAL');

            } catch (e) {
                document.getElementById('conn-pill').classList.remove('active');
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
    doc["temp"] = currentTemp;
    doc["humidity"] = currentHumidity;
    doc["distance"] = currentDistanceCm;
    doc["present"] = isPresent;
    doc["fan_pwm"] = currentFanPwmPercent;
    doc["mist_active"] = currentMistActive;
    doc["mode"] = (currentMode == MODE_AUTO) ? "auto" : "manual";
    doc["tropen_lock"] = tropenhausLockout;

    String jsonResponse;
    serializeJson(doc, jsonResponse);
    server.send(200, "application/json", jsonResponse);
}

void handleControlApi() {
    if (server.hasArg("mode")) {
        String modeArg = server.arg("mode");
        if (modeArg == "auto") {
            currentMode = MODE_AUTO;
        } else if (modeArg == "manual") {
            currentMode = MODE_MANUAL;
        }
    }

    if (currentMode == MODE_MANUAL) {
        if (server.hasArg("fan")) {
            int fanArg = server.arg("fan").toInt();
            setFanSpeedPercent(fanArg);
        }
        if (server.hasArg("mist")) {
            bool mistArg = (server.arg("mist") == "1" || server.arg("mist") == "true");
            setMistActive(mistArg);
        }
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

        // Zyklus: Auto (0) -> Leise 50% (1) -> Boost 100% (2) -> Aus (3)
        currentProfile = (ButtonProfile)((currentProfile + 1) % 4);

        switch (currentProfile) {
            case PROFILE_AUTO:
                currentMode = MODE_AUTO;
                break;
            case PROFILE_FAN_50:
                currentMode = MODE_MANUAL;
                setFanSpeedPercent(50);
                setMistActive(false);
                break;
            case PROFILE_BOOST:
                currentMode = MODE_MANUAL;
                setFanSpeedPercent(100);
                setMistActive(true);
                break;
            case PROFILE_OFF:
                currentMode = MODE_MANUAL;
                setFanSpeedPercent(0);
                setMistActive(false);
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

    // Pin-Modi konfigurieren
    pinMode(PIN_US_TRIG, OUTPUT);
    pinMode(PIN_US_ECHO, INPUT);
    pinMode(PIN_MIST_GATE, OUTPUT);
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    digitalWrite(PIN_MIST_GATE, LOW);

    // PWM für Arctic P12 Lüfter einrichten (25 kHz)
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
    ledcAttach(PIN_FAN_PWM, PWM_FREQ, PWM_RESOLUTION);
#else
    ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RESOLUTION);
    ledcAttachPin(PIN_FAN_PWM, PWM_CHANNEL);
#endif
    setFanSpeedPercent(0);

    // I2C und OLED Display starten
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println("OLED nicht gefunden!");
    }
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(15, 25);
    display.print("SMART COOLER...");
    display.display();

    // DHT22 starten
    dht.begin();

    // WLAN Access-Point starten (schul- & netzunabhängig)
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.print("WLAN Access Point aktiv. IP: ");
    Serial.println(WiFi.softAPIP());

    // Webserver Routen
    server.on("/", handleRoot);
    server.on("/api/status", handleStatusApi);
    server.on("/api/control", handleControlApi);
    server.begin();
    Serial.println("HTTP Webserver gestartet.");

    lastPresentTime = millis();
}

void loop() {
    server.handleClient();
    handleHardwareButton();

    unsigned long now = millis();

    // Sensoren alle 1000ms abfragen
    if (now - lastSensorRead >= 1000) {
        lastSensorRead = now;

        float t = dht.readTemperature();
        float h = dht.readHumidity();
        if (!isnan(t)) currentTemp = t;
        if (!isnan(h)) currentHumidity = h;

        long dist = readUltrasonicDistance();
        if (dist > 0 && dist < 400) {
            currentDistanceCm = dist;
        }

        // Wenn im Auto-Modus -> Logik auswerten
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
