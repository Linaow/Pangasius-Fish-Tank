#include <WiFi.h>
#include <WebServer.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ==============================================================================
// ⚙️ NETWORK CREDENTIALS
// ==============================================================================
const char* ssid = "*replace this with network name*";
const char* password = "*replace this with network password*";

// ==============================================================================
// 📍 PIN DEFINITIONS
// ==============================================================================
const int RELAY_AERATION = 25;
const int RELAY_FILTER = 26;
const int RELAY_HEAT_LAMP = 27;
const int ONE_WIRE_BUS = 14;   
const int PIN_TURBIDITY = 33;
const int PIN_TDS = 34;

// ==============================================================================
// 🎯 SPECIES SETPOINTS & CALIBRATION
// ==============================================================================
struct SpeciesProfile {
  String name;
  float targetTemp;
  float maxTurbidity;
  float tdsMin;
  float tdsMax;
};

SpeciesProfile tilapiaProfile = { "Tilapia", 28.0, 150.0, 300.0, 600.0 };
SpeciesProfile basaProfile    = { "Basa",    27.0, 120.0, 200.0, 500.0 };
SpeciesProfile zebraProfile   = { "Zebra",   27.0,  50.0, 100.0, 200.0 };

SpeciesProfile* activeProfile = &tilapiaProfile; 

const float TEMP_HYSTERESIS = 0.5;      
const float VOLTAGE_CLEAR_WATER = 2.40; 
const float VOLTAGE_BLIND = 1.60;       
const unsigned long SENSOR_READ_INTERVAL = 2000; 

// ==============================================================================
// 🌍 GLOBAL OBJECTS & VARIABLES
// ==============================================================================
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensor(&oneWire);
WebServer server(80);

float tempValue = -99.0; // Initialized to -99 to detect connection errors
float turbidityValue = 0.0;
float tdsValue = 0.0;

bool aerationStatus = false;
bool filterStatus = false;
bool heatLampStatus = false;
unsigned long previousMillis = 0; 

// ==============================================================================
// 🌐 FRONTEND HTML
// ==============================================================================
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Multi-Species Water Quality</title>
    <style>
        body { font-family: sans-serif; background-color: #121212; color: #ffffff; text-align: center; padding: 20px; }
        .tabs { display: flex; justify-content: center; gap: 10px; margin-bottom: 20px; }
        .tab-btn { background: #1e1e1e; color: #aaa; border: 2px solid #333; padding: 10px; cursor: pointer; border-radius: 10px; }
        .tab-btn.active { background: #00bcd4; color: #000; border-color: #00bcd4; }
        .card { background: #1e1e1e; padding: 15px; border-radius: 10px; margin-bottom: 10px; display: inline-block; width: 250px; }
        .value { font-size: 2rem; font-weight: bold; color: #00bcd4; }
        .status { padding: 5px 10px; border-radius: 5px; font-weight: bold; }
        .on { background: #4caf50; } .off { background: #f44336; }
    </style>
</head>
<body>
    <h1>💧 Water Monitor</h1>
    <div class="tabs">
        <button class="tab-btn active" id="tab-Tilapia" onclick="changeSpecies('Tilapia')">Tilapia</button>
        <button class="tab-btn" id="tab-Basa" onclick="changeSpecies('Basa')">Basa</button>
        <button class="tab-btn" id="tab-Zebra" onclick="changeSpecies('Zebra')">Zebra</button>
    </div>
    <div>
        <div class="card"><div>Temp</div><div class="value" id="temp">--</div></div>
        <div class="card"><div>Turbidity</div><div class="value" id="turbidity">--</div></div>
        <div class="card"><div>TDS</div><div class="value" id="tds">--</div></div>
    </div>
    <h3>Actuators</h3>
    <span class="status" id="s-heat">HEAT</span> 
    <span class="status" id="s-filter">FILTER</span> 
    <span class="status" id="s-air">AIR</span>
    <script>
        function changeSpecies(s) {
            document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
            document.getElementById('tab-' + s).classList.add('active');
            fetch('/set_species?s=' + s);
        }
        setInterval(() => {
            fetch('/data').then(r => r.json()).then(d => {
                document.getElementById('temp').innerText = d.temp.toFixed(1) + "°C";
                document.getElementById('turbidity').innerText = d.turbidity.toFixed(0) + " NTU";
                document.getElementById('tds').innerText = d.tds.toFixed(0) + " mg/L";
                document.getElementById('s-heat').className = "status " + (d.heat ? "on" : "off");
                document.getElementById('s-filter').className = "status " + (d.filter ? "on" : "off");
                document.getElementById('s-air').className = "status " + (d.aeration ? "on" : "off");
            });
        }, 2000);
    </script>
</body>
</html>
)rawliteral";

// ==============================================================================
// 🛠️ SETUP
// ==============================================================================
void setup() {
  Serial.begin(115200);
  
  // Initialize Temp Sensor
  tempSensor.begin();
  delay(500); 
  Serial.print("Scanning OneWire Pin 14... Found: ");
  Serial.println(tempSensor.getDeviceCount());

  // Initialize Relays (High = OFF for Active Low modules)
  pinMode(RELAY_AERATION, OUTPUT);
  pinMode(RELAY_FILTER, OUTPUT);
  pinMode(RELAY_HEAT_LAMP, OUTPUT);
  digitalWrite(RELAY_AERATION, HIGH); 
  digitalWrite(RELAY_FILTER, HIGH);   
  digitalWrite(RELAY_HEAT_LAMP, HIGH);

  // WiFi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  
  Serial.println("\nCONNECTED!");
  Serial.print("Click here: http://");
  Serial.println(WiFi.localIP());

  server.on("/", HTTP_GET, []() { server.send(200, "text/html", index_html); });
  server.on("/data", HTTP_GET, handleDataRequest);
  server.on("/set_species", HTTP_GET, handleSetSpecies);
  server.begin();
}

void loop() {
  server.handleClient(); 
  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= SENSOR_READ_INTERVAL) {
    previousMillis = currentMillis;
    readSensors();         
    evaluateLogic();       
    printSerialData();     
  }
}

// ==============================================================================
// 📊 LOGIC & HANDLERS
// ==============================================================================
void readSensors() {
  tempSensor.requestTemperatures(); 
  float realTemp = tempSensor.getTempCByIndex(0); 
  if (realTemp != DEVICE_DISCONNECTED_C) {
    tempValue = realTemp;
  } else {
    tempValue = -127.0; // Error indicator
  }

  // Simplified Turbidity
  float turbV = analogReadMilliVolts(PIN_TURBIDITY) / 1000.0;
  turbidityValue = map(turbV * 100, 160, 240, 3000, 0) / 1.0; 
  if(turbidityValue < 0) turbidityValue = 0;

  // TDS
  float tdsV = analogReadMilliVolts(PIN_TDS) / 1000.0;
  tdsValue = (133.42 * pow(tdsV, 3) - 255.86 * pow(tdsV, 2) + 857.39 * tdsV) * 0.5;
}

void evaluateLogic() {
  heatLampStatus = (tempValue < (activeProfile->targetTemp - TEMP_HYSTERESIS));
  filterStatus = (turbidityValue >= activeProfile->maxTurbidity);
  aerationStatus = (tdsValue < activeProfile->tdsMin || tdsValue > activeProfile->tdsMax);

  digitalWrite(RELAY_HEAT_LAMP, heatLampStatus ? LOW : HIGH);
  digitalWrite(RELAY_FILTER, filterStatus ? LOW : HIGH);
  digitalWrite(RELAY_AERATION, aerationStatus ? LOW : HIGH);
}

void handleSetSpecies() {
  if (server.hasArg("s")) {
    String s = server.arg("s");
    if (s == "Tilapia") activeProfile = &tilapiaProfile;
    else if (s == "Basa") activeProfile = &basaProfile;
    else if (s == "Zebra") activeProfile = &zebraProfile;
  }
  server.send(200, "text/plain", "OK");
}

void handleDataRequest() {
  String json = "{";
  json += "\"temp\":" + String(tempValue) + ",";
  json += "\"turbidity\":" + String(turbidityValue) + ",";
  json += "\"tds\":" + String(tdsValue) + ",";
  json += "\"aeration\":" + String(aerationStatus ? "true" : "false") + ",";
  json += "\"filter\":" + String(filterStatus ? "true" : "false") + ",";
  json += "\"heat\":" + String(heatLampStatus ? "true" : "false") + ",";
  json += "\"p_temp\":" + String(activeProfile->targetTemp) + ",";
  json += "\"p_turb\":" + String(activeProfile->maxTurbidity) + ",";
  json += "\"p_tds_min\":" + String(activeProfile->tdsMin) + ",";
  json += "\"p_tds_max\":" + String(activeProfile->tdsMax);
  json += "}";
  server.send(200, "application/json", json);
}
void printSerialData() {
 
// 1. Print Active Profile & IP Address
 Serial.print("[Profile: "); 
 Serial.print(activeProfile->name); 
 Serial.print("]  ");
 
  // This adds the clickable URL prefix before the IP
 Serial.print("Web UI: http://"); 
 Serial.print(WiFi.localIP()); 
 Serial.print("  ||  ");

  // 2. Print Sensor Readings
 Serial.print("SENSORS -> Temp: "); 
 Serial.print(tempValue, 1); 
 Serial.print("°C | Turbidity: "); 
 Serial.print(turbidityValue, 1); 
 Serial.print(" NTU | TDS: "); 
 Serial.print(tdsValue, 0); 
 Serial.print(" mg/L  ||  ");

  // 3. Print Actuator States
Serial.print("ACTUATORS -> Heat: "); Serial.print(heatLampStatus ? "ON" : "OFF");
Serial.print(" | Filter: "); 
Serial.print(filterStatus ? "ON" : "OFF");
Serial.print(" | Aeration: "); 
Serial.println(aerationStatus ? "ON" : "OFF"); 
}