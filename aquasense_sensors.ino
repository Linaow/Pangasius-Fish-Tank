/*
  AquaSense — Turbidity + DS18B20 Temperature
  =============================================
  Displays readings on Serial Monitor + hosted webpage

  Wiring:
  ── DS18B20 ──────────────────────────────────
    Red   (VCC)  → 3.3V
    Black (GND)  → GND
    Yellow(DATA) → GPIO 4
    * 4.7kΩ resistor between DATA and 3.3V (required)

  ── Turbidity Module ─────────────────────────
    Input  V → 5V  (VIN)
    Input  G → GND
    Output A → GPIO 34 (analog in)

  ── WiFi ─────────────────────────────────────
    Change SSID and PASSWORD below
*/

#include <WiFi.h>
#include <WebServer.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ── WiFi credentials ─────────────────────────
const char* SSID     = "Linah";
const char* PASSWORD = "Linahleeh";
// ─────────────────────────────────────────────

// ── Pin definitions ──────────────────────────
#define TEMP_PIN 4   // DS18B20 data pin
#define TURB_PIN     34   // Turbidity analog pin
#define LED_PIN      2
// ─────────────────────────────────────────────

OneWire oneWire(TEMP_PIN);
DallasTemperature tempSensor(&oneWire);
WebServer server(80);

float currentTemp     = 0;
float currentTurbNTU  = 0;
int   currentTurbRaw  = 0;

// Convert raw ADC (0-4095) to NTU (approximate)
// Calibrate this once you have clean vs dirty water readings
float rawToNTU(int raw) {
  // Turbidity sensors: higher voltage = cleaner water
  // Raw ADC is inverted — higher raw = lower turbidity
  float voltage = raw * (3.3 / 4095.0);
  if (voltage >= 2.5) return 0;
  float ntu = -1120.4 * voltage * voltage + 5742.3 * voltage - 4352.9;
  if (ntu < 0) ntu = 0;
  return ntu;
}

void readSensors() {
  // Temperature
  tempSensor.requestTemperatures();
  currentTemp = tempSensor.getTempCByIndex(0);

  // Turbidity — average 10 readings for stability
  long sum = 0;
  for (int i = 0; i < 10; i++) {
    sum += analogRead(TURB_PIN);
    delay(10);
  }
  currentTurbRaw = sum / 10;
  currentTurbNTU = rawToNTU(currentTurbRaw);
}

void handleRoot() {
  String html = R"rawhtml(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>AquaSense</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      background: #0a0f1a; color: #f0f4ff;
      font-family: sans-serif;
      display: flex; flex-direction: column;
      align-items: center; padding: 2rem;
      min-height: 100vh;
    }
    h1 { font-size: 1rem; color: #6b7a99; letter-spacing: 3px;
         text-transform: uppercase; margin-bottom: 2rem; }
    .grid { display: grid; grid-template-columns: 1fr 1fr;
            gap: 1rem; width: 100%; max-width: 500px; }
    .card { background: #111827;
            border: 1px solid rgba(255,255,255,0.07);
            border-radius: 16px; padding: 1.5rem;
            text-align: center; }
    .label { font-size: 10px; color: #6b7a99; letter-spacing: 2px;
             text-transform: uppercase; margin-bottom: 0.5rem; }
    .value { font-size: 2.5rem; font-weight: 700;
             font-family: monospace; line-height: 1; }
    .unit  { font-size: 0.8rem; color: #6b7a99; margin-top: 0.3rem; }
    .temp-val  { color: #f97316; }
    .turb-val  { color: #a78bfa; }
    .raw-val   { color: #6b7a99; font-size: 1.2rem; }
    .status { margin-top: 2rem; font-size: 12px; color: #6b7a99;
              display: flex; align-items: center; gap: 6px; }
    .dot { width: 8px; height: 8px; border-radius: 50%;
           background: #22c55e; animation: pulse 1.5s infinite; }
    @keyframes pulse { 0%,100%{opacity:1} 50%{opacity:0.3} }
  </style>
</head>
<body>
  <h1>AquaSense — Live Readings</h1>
  <div class="grid">
    <div class="card">
      <div class="label">Temperature</div>
      <div class="value temp-val" id="temp">--</div>
      <div class="unit">°C</div>
    </div>
    <div class="card">
      <div class="label">Turbidity</div>
      <div class="value turb-val" id="turb">--</div>
      <div class="unit">NTU</div>
    </div>
    <div class="card">
      <div class="label">Turb Raw ADC</div>
      <div class="value raw-val" id="raw">--</div>
      <div class="unit">0 – 4095</div>
    </div>
    <div class="card">
      <div class="label">Temp Status</div>
      <div class="value" id="tempst" style="font-size:1.4rem">--</div>
      <div class="unit">for Basa fish</div>
    </div>
  </div>
  <div class="status"><div class="dot"></div>Updates every 2 seconds</div>

  <script>
    async function update() {
      try {
        const r = await fetch('/reading');
        const d = await r.json();
        document.getElementById('temp').textContent = d.temperature.toFixed(1);
        document.getElementById('turb').textContent = d.turbidity_ntu.toFixed(1);
        document.getElementById('raw').textContent  = d.turbidity_raw;

        // Simple Basa fish temp check (26–30°C)
        const t = d.temperature;
        const el = document.getElementById('tempst');
        if (t < 26) { el.textContent = 'LOW'; el.style.color = '#3b82f6'; }
        else if (t > 30) { el.textContent = 'HIGH'; el.style.color = '#ef4444'; }
        else { el.textContent = 'OK'; el.style.color = '#22c55e'; }
      } catch(e) {}
    }
    update();
    setInterval(update, 2000);
  </script>
</body>
</html>
)rawhtml";
  server.send(200, "text/html", html);
}

void handleReading() {
  String json = "{";
  json += "\"temperature\":"    + String(currentTemp, 2)    + ",";
  json += "\"turbidity_ntu\":"  + String(currentTurbNTU, 2) + ",";
  json += "\"turbidity_raw\":"  + String(currentTurbRaw);
  json += "}";
  server.send(200, "application/json", json);
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  pinMode(TURB_PIN, INPUT);
  digitalWrite(TEMP_PIN, HIGH);
  tempSensor.begin();

  Serial.println("\nConnecting to WiFi...");
  WiFi.begin(SSID, PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }
  digitalWrite(LED_PIN, HIGH);
  Serial.println("\nWiFi Connected!");
  Serial.print("Open on your phone: http://");
  Serial.println(WiFi.localIP());

  server.on("/", handleRoot);
  server.on("/reading", handleReading);
  server.begin();
}

void loop() {
  readSensors();

  // Serial monitor output
  Serial.println("─────────────────────────");
  Serial.printf("Temperature  : %.2f °C\n", currentTemp);
  Serial.printf("Turbidity    : %.1f NTU\n", currentTurbNTU);
  Serial.printf("Turb Raw ADC : %d\n", currentTurbRaw);
  Serial.println("─────────────────────────");

  server.handleClient();
  delay(2000);
}
