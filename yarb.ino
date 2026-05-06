#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>
#include <math.h>

// ==============================================================================
// CONFIGURATION
// ==============================================================================
const char* ssid     = "";
const char* password = "";

#define DATABASE_URL    "p"
#define DATABASE_SECRET ""

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
// SPECIES PROFILES — each has its own hysteresis
// ==============================================================================
struct SpeciesProfile {
  const char* name;
  float targetTemp;
  float hysteresis;
  float maxTurbidity;
  float tdsMin;
  float tdsMax;
};

SpeciesProfile pangasiusProfile = { "Pangasius",  28.0, 1.0, 80.0,  100.0, 400.0 };
SpeciesProfile tilapiaProfile   = { "Tilapia",    30.0, 2.0, 100.0, 200.0, 500.0 };
SpeciesProfile zebraFishProfile = { "Zebra Fish", 26.0, 0.5, 30.0,  50.0,  150.0 };

SpeciesProfile* activeProfile = &tilapiaProfile;

// ==============================================================================
// CONSTANTS & GLOBALS
// ==============================================================================
const unsigned long SEND_INTERVAL  = 5000;
const unsigned long READ_INTERVAL  = 2000;
const unsigned long DEBUG_INTERVAL = 2000;
const int           NUM_READINGS   = 10;

FirebaseData   fbdo;
FirebaseAuth   auth;
FirebaseConfig config;

OneWire           oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensor(&oneWire);

// Sensor values
float tempValue      = 25.0;
float turbidityValue = 0.0;
float tdsValue       = 0.0;

// Raw voltages — declared here so printDebug() can access them
float turbRaw_V  = 0.0;
float tdsRaw_V   = 0.0;
float tdsKFactor = 0.5;

// Actuator states
bool aerationStatus       = false;
bool filterStatus         = false;
bool heatLampStatus       = false;
bool manualOverrideActive = false;

// Timers
unsigned long lastMsg        = 0;
unsigned long lastSensorRead = 0;
unsigned long lastDebugPrint = 0;

// Rolling average buffers
float turbidityBuf[NUM_READINGS] = {0};
float tdsBuf[NUM_READINGS]       = {0};
int   turbidityIndex = 0;
int   tdsIndex       = 0;

// Turbidity calibration
const float TURBIDITY_CLEAN_V   = 3.1;
const float TURBIDITY_SLOPE_NTU = 1000.0;

// Change detection for debug output
String lastSpeciesName = "";
bool   lastManualMode  = false;
bool   lastHeat        = false;
bool   lastFilter      = false;
bool   lastAir         = false;
bool   firstDebug      = true;

// ==============================================================================
// HELPERS
// ==============================================================================
String relayState(bool on) { return on ? "ON " : "OFF"; }

String tempStatus(float temp, SpeciesProfile* p) {
  if (temp < p->targetTemp - p->hysteresis) return "LOW  heating UP ";
  if (temp > p->targetTemp + p->hysteresis) return "HIGH cooling DN ";
  return "OK   in range   ";
}

String tdsStatus(float tds, SpeciesProfile* p) {
  if (tds < p->tdsMin) return "LOW  below range";
  if (tds > p->tdsMax) return "HIGH above range";
  return "OK   in range   ";
}

String turbStatus(float turb, SpeciesProfile* p) {
  if (turb > p->maxTurbidity) return "HIGH too cloudy ";
  return "OK   clear      ";
}

// ==============================================================================
// WIFI & FIREBASE
// ==============================================================================
void connectToWiFi() {
  Serial.println();
  Serial.println("╔══════════════════════════════════════════╗");
  Serial.println("║         AquaCore ESP32  v2.0             ║");
  Serial.println("╚══════════════════════════════════════════╝");
  Serial.printf ("  Connecting to WiFi: %s\n", ssid);
  WiFi.begin(ssid, password);
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    attempt++;
    if (attempt > 40) {
      Serial.println("\n  [ERROR] WiFi timeout — restarting...");
      ESP.restart();
    }
  }
  Serial.println();
  Serial.printf("  Connected — IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("  Signal:  %d dBm\n", WiFi.RSSI());
}

void setupFirebase() {
  Serial.println("  Connecting to Firebase...");
  config.database_url = DATABASE_URL;
  config.signer.tokens.legacy_token = DATABASE_SECRET;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  Serial.println("  Firebase ready");
  Serial.println("════════════════════════════════════════════");
}

// ==============================================================================
// SENSOR READS
// ==============================================================================
float readTemperature() {
  tempSensor.requestTemperatures();
  float t = tempSensor.getTempCByIndex(0);
  if (t <= -100.0 || t == 85.0 || isnan(t)) {
    Serial.println("  [WARN] Temp sensor error — using last value");
    return tempValue;
  }
  return t;
}

float readTurbidity() {
  // store raw voltage in global so debug can print it
  turbRaw_V = analogReadMilliVolts(PIN_TURBIDITY) / 1000.0f;
  float ntu = TURBIDITY_SLOPE_NTU * (TURBIDITY_CLEAN_V - turbRaw_V) / TURBIDITY_CLEAN_V;
  ntu = constrain(ntu, 0.0f, 3000.0f);
  turbidityBuf[turbidityIndex] = ntu;
  turbidityIndex = (turbidityIndex + 1) % NUM_READINGS;
  float sum = 0;
  for (int i = 0; i < NUM_READINGS; i++) sum += turbidityBuf[i];
  return sum / NUM_READINGS;
}

float readTDS() {
  // store raw voltage in global so debug can print it
  tdsRaw_V = analogReadMilliVolts(PIN_TDS) / 1000.0f;
  float compV  = tdsRaw_V / (1.0f + 0.02f * (tempValue - 25.0f));
  float tdsRaw = 133.42f * powf(compV, 3) - 255.86f * powf(compV, 2) + 857.39f * compV;
  float tdsFinal = max(tdsRaw * tdsKFactor, 0.0f);
  tdsBuf[tdsIndex] = tdsFinal;
  tdsIndex = (tdsIndex + 1) % NUM_READINGS;
  float sum = 0;
  for (int i = 0; i < NUM_READINGS; i++) sum += tdsBuf[i];
  return sum / NUM_READINGS;
}

void readSensors() {
  tempValue      = readTemperature();
  turbidityValue = readTurbidity();
  tdsValue       = readTDS();
}

// ==============================================================================
// CONTROL LOGIC
// ==============================================================================
void evaluateLogic() {
  if (!manualOverrideActive) {
    // Heater — hysteresis band from species profile
    if      (tempValue < activeProfile->targetTemp - activeProfile->hysteresis) heatLampStatus = true;
    else if (tempValue > activeProfile->targetTemp + activeProfile->hysteresis) heatLampStatus = false;

    // Filter — TDS or turbidity out of range
    filterStatus = (tdsValue > activeProfile->tdsMax) ||
                   (turbidityValue > activeProfile->maxTurbidity);

    // Aerator — always ON in aquaculture
    aerationStatus = true;
  }

  // Relays are active-LOW
  digitalWrite(RELAY_HEAT_LAMP, heatLampStatus ? LOW : HIGH);
  digitalWrite(RELAY_FILTER,    filterStatus   ? LOW : HIGH);
  digitalWrite(RELAY_AERATION,  aerationStatus ? LOW : HIGH);
}

// ==============================================================================
// SERIAL DEBUG
// ==============================================================================
void printDebug() {
  bool stateChanged = firstDebug ||
    (lastSpeciesName != String(activeProfile->name)) ||
    (lastManualMode  != manualOverrideActive)         ||
    (lastHeat        != heatLampStatus)               ||
    (lastFilter      != filterStatus)                 ||
    (lastAir         != aerationStatus);

  Serial.println();
  Serial.println("┌──────────────────────────────────────────────┐");
  Serial.printf( "│  AquaCore  ·  uptime: %lus%*s│\n",
    millis()/1000, (int)(22 - String(millis()/1000).length()), " ");
  Serial.println("├──────────────────────────────────────────────┤");
  Serial.printf( "│  Species : %-34s│\n", activeProfile->name);
  if (manualOverrideActive) {
    Serial.println("│  Mode    : MANUAL  ◄ dashboard override       │");
  } else {
    Serial.println("│  Mode    : AUTO    ◄ closed-loop ESP32        │");
  }
  Serial.println("├──────────────┬─────────────────┬─────────────┤");
  Serial.println("│  SENSOR      │  VALUE / TARGET │  STATUS     │");
  Serial.println("├──────────────┼─────────────────┼─────────────┤");

  // Temperature
  char tLine[20];
  sprintf(tLine, "%.2f / %.1f C", tempValue, activeProfile->targetTemp);
  Serial.printf("│  Temp        │ %-15s │ %s│\n", tLine, tempStatus(tempValue, activeProfile).c_str());

  // TDS
  char dLine[20];
  sprintf(dLine, "%.0f ppm", tdsValue);
  char dRange[20];
  sprintf(dRange, "%.0f-%.0f", activeProfile->tdsMin, activeProfile->tdsMax);
  Serial.printf("│  TDS (%s)  │ %-15s │ %s│\n", dRange, dLine, tdsStatus(tdsValue, activeProfile).c_str());

  // Turbidity
  char uLine[20];
  sprintf(uLine, "%.1f / %.0f NTU", turbidityValue, activeProfile->maxTurbidity);
  Serial.printf("│  Turbidity   │ %-15s │ %s│\n", uLine, turbStatus(turbidityValue, activeProfile).c_str());

  Serial.println("├──────────────┴─────────────────┴─────────────┤");
  Serial.println("│  RAW VOLTAGES  (for calibration)             │");
  Serial.println("├────────────────────┬─────────────────────────┤");
  Serial.printf( "│  Turbidity ADC     │  %.4f V                │\n", turbRaw_V);
  Serial.printf( "│  TDS ADC           │  %.4f V                │\n", tdsRaw_V);
  Serial.printf( "│  TDS K-Factor      │  %.4f                  │\n", tdsKFactor);
  Serial.println("├────────────────────┴─────────────────────────┤");
  Serial.println("│  ACTUATORS                                   │");
  Serial.println("├────────────────┬─────────────────────────────┤");

  // Print actuator state + flag if it just changed
  bool heatChanged   = !firstDebug && (lastHeat   != heatLampStatus);
  bool filterChanged = !firstDebug && (lastFilter != filterStatus);
  bool airChanged    = !firstDebug && (lastAir    != aerationStatus);

  Serial.printf("│  Heater        │  %s  %s│\n",
    relayState(heatLampStatus).c_str(),
    heatChanged   ? "◄ CHANGED              " : "                       ");
  Serial.printf("│  Filter        │  %s  %s│\n",
    relayState(filterStatus).c_str(),
    filterChanged ? "◄ CHANGED              " : "                       ");
  Serial.printf("│  Aerator       │  %s  %s│\n",
    relayState(aerationStatus).c_str(),
    airChanged    ? "◄ CHANGED              " : "                       ");

  Serial.println("├────────────────┴─────────────────────────────┤");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("│  WiFi  : CONNECTED   RSSI: %d dBm          │\n", WiFi.RSSI());
  } else {
    Serial.println("│  WiFi  : DISCONNECTED                        │");
  }
  Serial.printf( "│  Firebase : %-32s│\n", Firebase.ready() ? "READY" : "NOT READY");
  Serial.println("└──────────────────────────────────────────────┘");

  // Save for next comparison
  lastSpeciesName = String(activeProfile->name);
  lastManualMode  = manualOverrideActive;
  lastHeat        = heatLampStatus;
  lastFilter      = filterStatus;
  lastAir         = aerationStatus;
  firstDebug      = false;
}

// ==============================================================================
// FIREBASE SYNC
// ==============================================================================
void syncFirebase() {
  if (!Firebase.ready()) {
    Serial.println("  [Firebase] Not ready — skipping sync");
    return;
  }

  // Write status to Firebase
  FirebaseJson json;
  json.add("temp",         tempValue);
  json.add("turbidity",    turbidityValue);
  json.add("tds",          tdsValue);
  json.add("heat",         heatLampStatus);
  json.add("filter",       filterStatus);
  json.add("air",          aerationStatus);
  json.add("species_name", activeProfile->name);

  if (!Firebase.RTDB.setJSON(&fbdo, "/aquarium/status", &json)) {
    Serial.printf("  [Firebase] Write error: %s\n", fbdo.errorReason().c_str());
  }

  // Read controls from Firebase
  if (Firebase.RTDB.getJSON(&fbdo, "/aquarium/controls")) {
    FirebaseJson    &data = fbdo.jsonObject();
    FirebaseJsonData res;

    // Override mode
    data.get(res, "override");
    if (res.success) {
      bool newOverride = res.boolValue;
      if (newOverride != manualOverrideActive) {
        manualOverrideActive = newOverride;
        Serial.printf("  [Firebase] Mode → %s\n",
          manualOverrideActive ? "MANUAL (dashboard)" : "AUTO (closed-loop)");
      }
    }

    // Manual actuator commands
    if (manualOverrideActive) {
      data.get(res, "heat");
      if (res.success && res.boolValue != heatLampStatus) {
        heatLampStatus = res.boolValue;
        Serial.printf("  [Firebase] Heater  → %s\n", relayState(heatLampStatus).c_str());
      }
      data.get(res, "filter");
      if (res.success && res.boolValue != filterStatus) {
        filterStatus = res.boolValue;
        Serial.printf("  [Firebase] Filter  → %s\n", relayState(filterStatus).c_str());
      }
      data.get(res, "air");
      if (res.success && res.boolValue != aerationStatus) {
        aerationStatus = res.boolValue;
        Serial.printf("  [Firebase] Aerator → %s\n", relayState(aerationStatus).c_str());
      }
    }

    // Species selection
    data.get(res, "species");
    if (res.success) {
      SpeciesProfile* newProfile = &tilapiaProfile;
      if      (res.stringValue == "pangasius")  newProfile = &pangasiusProfile;
      else if (res.stringValue == "zebra_fish") newProfile = &zebraFishProfile;

      if (newProfile != activeProfile) {
        activeProfile = newProfile;
        Serial.printf("  [Firebase] Species → %s\n", activeProfile->name);
      }
    }
  } else {
    Serial.printf("  [Firebase] Read error: %s\n", fbdo.errorReason().c_str());
  }
}

// ==============================================================================
// SETUP
// ==============================================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  tempSensor.begin();

  pinMode(RELAY_AERATION,  OUTPUT); digitalWrite(RELAY_AERATION,  HIGH);
  pinMode(RELAY_FILTER,    OUTPUT); digitalWrite(RELAY_FILTER,    HIGH);
  pinMode(RELAY_HEAT_LAMP, OUTPUT); digitalWrite(RELAY_HEAT_LAMP, HIGH);

  analogSetAttenuation(ADC_11db);
  analogReadResolution(12);

  connectToWiFi();
  setupFirebase();

  Serial.println("  Active profile : Tilapia (default)");
  Serial.println("  System ready   — entering main loop");
  Serial.println();
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
    syncFirebase();
  }

  if (now - lastDebugPrint > DEBUG_INTERVAL) {
    lastDebugPrint = now;
    printDebug();
  }
}
