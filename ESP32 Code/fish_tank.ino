#include <WiFi.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>
#include <math.h>

// ==============================================================================
// CONFIGURATION
//change the data into your network dat
// ==============================================================================
const char* ssid       = "network_name";
const char* password   = "network_password";
const char* server_url = "";  

// ==============================================================================
// PIN DEFINITIONS
// ==============================================================================
const int RELAY_AERATION  = 25;
const int RELAY_FILTER    = 26;
const int RELAY_HEAT_LAMP = 27;
const int ONE_WIRE_BUS    = 14;
const int PIN_TURBIDITY   = 33;
const int PIN_TDS         = 34;

// ==============================================================================
// SPECIES PROFILE
// ==============================================================================
struct SpeciesProfile {
  const char* name;
  float targetTemp;
  float maxTurbidity;
  float tdsMin;
  float tdsMax;
};
//SpeciesProfile Profile = { "Species",  temp C, Turbidity NTU, TDS Min , TDS Max };
SpeciesProfile pangasiusProfile = { "Pangasius",  28.0, 100.0, 100.0, 400.0 };
SpeciesProfile tilapiaProfile   = { "Tilapia",    30.0, 80.0, 200.0, 500.0 };
SpeciesProfile zebraFishProfile = { "Zebra Fish", 25.0, 30.0, 50.0, 150.0 };


SpeciesProfile* activeProfile = &tilapiaProfile;

// ==============================================================================
// CONSTANTS
// ==============================================================================
const float         TEMP_HYSTERESIS = 0.5;
const unsigned long SEND_INTERVAL   = 5000;
const unsigned long READ_INTERVAL   = 2000;
const int           NUM_READINGS    = 10;

// ==============================================================================
// CALIBRATION
// ==============================================================================
float tdsKFactor = 0.5;

const float TURBIDITY_CLEAN_V   = 3.1;
const float TURBIDITY_SLOPE_NTU = 1000.0;

// ==============================================================================
// GLOBAL STATE
// ==============================================================================
OneWire           oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensor(&oneWire);

float tempValue      = 25.0;
float turbidityValue = 0.0;
float tdsValue       = 0.0;

bool aerationStatus = false;
bool filterStatus   = false;
bool heatLampStatus = false;
bool manualOverrideActive = false;

unsigned long lastMsg        = 0;
unsigned long lastSensorRead = 0;

float turbidityBuf[NUM_READINGS] = {0};
float tdsBuf[NUM_READINGS]       = {0};
int   turbidityIndex = 0;
int   tdsIndex       = 0;

// ==============================================================================
// WIFI
// ==============================================================================
void connectToWiFi() {
  Serial.println(">> Connecting to WiFi...");
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(">> WiFi connected. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("!! WiFi FAILED — check SSID/password or router.");
  }
}

// ==============================================================================
// SENSOR READS
// ==============================================================================
float readTemperature() {
  tempSensor.requestTemperatures();
  float t = tempSensor.getTempCByIndex(0);
  if (t <= -100.0 || t == 85.0 || isnan(t)) {
    Serial.println("!! Temp sensor error — holding last value.");
    return tempValue;
  }
  return t;
}

float readTurbidity() {
  float voltage = analogReadMilliVolts(PIN_TURBIDITY) / 1000.0f;
  float ntu = TURBIDITY_SLOPE_NTU * (TURBIDITY_CLEAN_V - voltage) / TURBIDITY_CLEAN_V;
  ntu = constrain(ntu, 0.0f, 3000.0f);

  turbidityBuf[turbidityIndex] = ntu;
  turbidityIndex = (turbidityIndex + 1) % NUM_READINGS;

  float sum = 0;
  for (int i = 0; i < NUM_READINGS; i++) sum += turbidityBuf[i];
  return sum / NUM_READINGS;
}

float readTDS() {
  float rawV = analogReadMilliVolts(PIN_TDS) / 1000.0f;
  float compV = rawV / (1.0f + 0.02f * (tempValue - 25.0f));
  float tdsRaw = 133.42f * powf(compV, 3)
               - 255.86f * powf(compV, 2)
               + 857.39f * compV;
  float tdsFinal = max(tdsRaw * tdsKFactor, 0.0f);

  tdsBuf[tdsIndex] = tdsFinal;
  tdsIndex = (tdsIndex + 1) % NUM_READINGS;

  float sum = 0;
  for (int i = 0; i < NUM_READINGS; i++) sum += tdsBuf[i];
  return sum / NUM_READINGS;
}

// ==============================================================================
// READ ALL SENSORS + DEBUG OUTPUT
// ==============================================================================
void readSensors() {
  tempValue      = readTemperature();
  turbidityValue = readTurbidity();
  tdsValue       = readTDS();

  float turbRaw_V = analogRead(PIN_TURBIDITY) * (3.3f / 4095.0f);
  float tdsRaw_V  = analogReadMilliVolts(PIN_TDS) / 1000.0f;

  Serial.println("=============================");
  Serial.printf("Species:     %s\n",   activeProfile->name);
  Serial.printf("Temp:        %.2f C  (target %.1f C)\n", tempValue, activeProfile->targetTemp);
  Serial.printf("Turb NTU:    %.1f    (max %.0f NTU)\n",  turbidityValue, activeProfile->maxTurbidity);
  Serial.printf("TDS ppm:     %.0f    (range %.0f–%.0f)\n", tdsValue, activeProfile->tdsMin, activeProfile->tdsMax);
  Serial.printf("Turb Raw V:  %.4f V\n", turbRaw_V);
  Serial.printf("TDS Raw V:   %.4f V\n", tdsRaw_V);
  Serial.printf("K-Factor:    %.4f\n",   tdsKFactor);
  Serial.println("=============================");
}

// ==============================================================================
// CONTROL LOGIC
// ==============================================================================
void evaluateLogic() {
  if (!manualOverrideActive) {
    // Heater hysteresis
    if (tempValue < activeProfile->targetTemp - TEMP_HYSTERESIS) {
      heatLampStatus = true;
    } else if (tempValue > activeProfile->targetTemp + TEMP_HYSTERESIS) {
      heatLampStatus = false;
    }

    // Filter
    bool tdsHigh  = tdsValue       > activeProfile->tdsMax;
    bool turbHigh = turbidityValue > activeProfile->maxTurbidity;
    filterStatus  = tdsHigh || turbHigh;

    // Aerator
    bool tempHigh   = tempValue > activeProfile->targetTemp + 2.0f;
    aerationStatus  = filterStatus || tempHigh;
  }

  // Apply to relays (LOW = relay energised on most relay boards)
  digitalWrite(RELAY_HEAT_LAMP, heatLampStatus ? LOW : HIGH);
  digitalWrite(RELAY_FILTER,    filterStatus   ? LOW : HIGH);
  digitalWrite(RELAY_AERATION,  aerationStatus ? LOW : HIGH);

  Serial.printf(">> Control — Heater:%s  Filter:%s  Aerator:%s  Mode:%s\n",
    heatLampStatus ? "ON" : "OFF",
    filterStatus   ? "ON" : "OFF",
    aerationStatus ? "ON" : "OFF",
    manualOverrideActive ? "MANUAL" : "AUTO");
}

// ==============================================================================
// NODE-RED
// ==============================================================================
void sendToNodeRed() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(server_url);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> doc;
  doc["temp"]      = tempValue;
  doc["turbidity"] = turbidityValue;
  doc["tds"]       = tdsValue;
  doc["heat"]      = heatLampStatus;
  doc["filter"]    = filterStatus;
  doc["air"]       = aerationStatus;

  String payload;
  serializeJson(doc, payload);

  int code = http.POST(payload);

  if (code == 200) {
    String response = http.getString();

    StaticJsonDocument<256> inbound;
    DeserializationError err = deserializeJson(inbound, response);
    if (err) {
      Serial.println("!! JSON parse error on Node-RED response");
      http.end();
      return;
    }

    // Species override from dashboard
    if (inbound.containsKey("species")) {
      const char* sp = inbound["species"];
      if      (strcmp(sp, "pangasius")  == 0) activeProfile = &pangasiusProfile;
      else if (strcmp(sp, "zebra_fish") == 0) activeProfile = &zebraFishProfile;
      else                                     activeProfile = &tilapiaProfile;
      Serial.printf(">> Profile updated → %s\n", activeProfile->name);
    }

    // Manual override from dashboard
    if (inbound.containsKey("override")) {
      bool ov = inbound["override"];
      if (ov) {
        manualOverrideActive = true;
        if (inbound.containsKey("heat"))   heatLampStatus = inbound["heat"];
        if (inbound.containsKey("filter")) filterStatus   = inbound["filter"];
        if (inbound.containsKey("air"))    aerationStatus = inbound["air"];
        // Apply immediately
        digitalWrite(RELAY_HEAT_LAMP, heatLampStatus ? LOW : HIGH);
        digitalWrite(RELAY_FILTER,    filterStatus   ? LOW : HIGH);
        digitalWrite(RELAY_AERATION,  aerationStatus ? LOW : HIGH);
        Serial.println(">> Manual override APPLIED");
      } else {
        if (manualOverrideActive) {
          manualOverrideActive = false;
          Serial.println(">> Manual override CLEARED — resuming auto");
        }
      }
    }

  } else {
    Serial.printf("!! Node-RED POST failed, HTTP code: %d\n", code);
  }
  http.end();
}

// ==============================================================================
// TDS CALIBRATION
// ==============================================================================
void calibrateTDS(float knownPPM) {
  Serial.printf(">> Calibrating TDS with %.0f ppm solution...\n", knownPPM);
  float sumV = 0;
  for (int i = 0; i < 20; i++) {
    sumV += analogReadMilliVolts(PIN_TDS) / 1000.0f;
    delay(50);
  }
  float avgV  = sumV / 20.0f;
  float compV = avgV / (1.0f + 0.02f * (tempValue - 25.0f));
  float base  = 133.42f * powf(compV, 3)
              - 255.86f * powf(compV, 2)
              + 857.39f * compV;
  if (base <= 0) {
    Serial.println("!! Calibration failed — base TDS is 0. Is probe submerged?");
    return;
  }
  float oldFactor = tdsKFactor;
  tdsKFactor = knownPPM / base;
  Serial.printf(">> Old K-Factor: %.4f\n", oldFactor);
  Serial.printf(">> New K-Factor: %.4f  ← hardcode this to survive reboot\n", tdsKFactor);
}

// ==============================================================================
// SETUP
// ==============================================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Aquarium Control System ===");

  tempSensor.begin();

  pinMode(RELAY_AERATION,  OUTPUT); digitalWrite(RELAY_AERATION,  HIGH);
  pinMode(RELAY_FILTER,    OUTPUT); digitalWrite(RELAY_FILTER,    HIGH);
  pinMode(RELAY_HEAT_LAMP, OUTPUT); digitalWrite(RELAY_HEAT_LAMP, HIGH);

  analogSetAttenuation(ADC_11db);
  analogReadResolution(12);

  connectToWiFi();
  delay(2000);
  Serial.println(">> System ready.\n");
}

// ==============================================================================
// LOOP
// ==============================================================================
void loop() {
  unsigned long now = millis();

  if (now - lastSensorRead > READ_INTERVAL) {
    lastSensorRead = now;
    readSensors();
    evaluateLogic();
  }

  if (now - lastMsg > SEND_INTERVAL) {
    lastMsg = now;
    sendToNodeRed();
  }

  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.startsWith("CAL ")) {
      float val = cmd.substring(4).toFloat();
      if (val > 0) calibrateTDS(val);
      else Serial.println("!! Usage: CAL 500");
    }
  }
}