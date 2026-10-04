#include <WiFi.h>
#include <WebServer.h>
#include <ModbusMaster.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <PubSubClient.h>

// --- WLAN Konfiguration ---
const char* ssid = "DEIN_WLAN_NAME";
const char* password = "DEIN_WLAN_PASSWORT";

// --- MQTT Konfiguration ---
const char* mqtt_server = "192.168.1.100";  // IP deines MQTT-Brokers
const int   mqtt_port   = 1883;
const char* mqtt_user   = "";               // Optional (sonst leer lassen)
const char* mqtt_pass   = "";               // Optional (sonst leer lassen)
const char* mqtt_prefix = "heatpump";       // Topic-Präfix
const char* mqtt_client_id = "BWT_HeatPump"; // Feste MQTT Client-ID

// --- Hardware Pins (ESP32-C3) ---
#define RX_PIN 2
#define TX_PIN 3

// --- Betriebsmodi der Wärmepumpe (Register 0x03E8) ---
enum HeatPumpMode {
    MODE_OFF        = 0,   // Aus
    MODE_OFF_PRESET = 2,   // Zwischenschritt beim Ausschalten
    MODE_AUTO       = 17,  // Auto (Heizen & Kühlen)
    MODE_COOL       = 18,  // Kühlen
    MODE_SMART      = 21,  // Smart Heizen
    MODE_BOOST      = 22,  // Boost Mode
    MODE_ECO        = 23   // Eco Heizen
};

WiFiClient espClient;
PubSubClient mqttClient(espClient);

WebServer server(80);
ModbusMaster node;
HardwareSerial ModbusSerial(1);

// --- Web-Logger (500 Zeilen Ringpuffer) ---
const int MAX_LOG_LINES = 300;
String logBuffer[MAX_LOG_LINES];
int logIndex = 0;

// Erzeugt den Zeitstempel [HH:MM] mit automatischer Sommer-/Winterzeit
String getTimeStr() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10)) { // Timeout 10ms
    return "[--:--] "; // Fallback, falls Uhrzeit noch nicht via WLAN empfangen wurde
  }
  char timeBuf[10];
  strftime(timeBuf, sizeof(timeBuf), "[%H:%M] ", &timeinfo);
  return String(timeBuf);
}

void addLog(String msg) {
  String timeStampedMsg = getTimeStr() + msg;
  Serial.println(timeStampedMsg); 
  logBuffer[logIndex] = timeStampedMsg;
  logIndex = (logIndex + 1) % MAX_LOG_LINES;
}

// --- Dynamic Register Cache ---
struct RegisterState {
  uint16_t addr;
  uint16_t val;
};
RegisterState regCache[128];
int regCacheCount = 0;

// --- Live-Anzeige Datenstruktur ---
struct PWP_State {
  uint16_t raw_mode = 0;
  float target_temp = 28.0;
  float water_in_temp = 0.0;
  float water_out_temp = 0.0;
  float outdoor_temp = 0.0;
  float compressor_freq = 0.0;
  float suction_temp = 0.0;
  float hotgas_temp = 0.0;
  float evap_temp = 0.0;
  uint16_t fan_status = 0;
} pwp;

// --- Formatierungs-Hilfsfunktionen ---
String hex2(uint8_t v) {
  char tmp[5];
  sprintf(tmp, "%02X", v);
  return String(tmp);
}

String hex4(uint16_t v) {
  char tmp[7];
  sprintf(tmp, "0x%04X", v);
  return String(tmp);
}

String bufferToHex(uint8_t *buf, uint16_t len) {
  String hexStr = "";
  for (uint16_t i = 0; i < len; i++) {
    hexStr += hex2(buf[i]);
    if (i < len - 1) hexStr += " ";
  }
  return hexStr;
}

String formatReqFC03(uint8_t* buf) {
  return hex2(buf[0]) + "|" + hex2(buf[1]) + "|" + hex2(buf[2]) + " " + hex2(buf[3]) + "|" + hex2(buf[4]) + " " + hex2(buf[5]);
}

String formatRespFC03(uint8_t* buf, uint16_t len) {
  String s = hex2(buf[0]) + "|" + hex2(buf[1]) + "|" + hex2(buf[2]) + "|";
  for(int i = 3; i < len; i++) {
    s += hex2(buf[i]);
    if(i < len - 1) s += " ";
  }
  return s;
}

String getRegisterName(uint16_t addr) {
  switch(addr) {
    case 0x0203: return "Außentemp";
    case 0x03E9: return "Soll-Temp";
    case 0x03E8: return "Betriebsmodus";
    case 0x0200: return "Vorlauf-Temp";
    case 0x0201: return "Rücklauf-Temp";
    case 0x0044: return "Heißgas-Temp";
    case 0x0209: return "Saugrohr-Temp";
    case 0x020A: return "Verdampfer-Temp";
    case 0x01FE: return "Kompressor-Freq";
    case 0x01F4: return "Status/Lüfter (0x01F4)";
    default:     return "Register " + hex4(addr);
  }
}

String decodeRegValue(uint16_t addr, uint16_t val) {
  char hexVal[7];
  sprintf(hexVal, "0x%04X", val);

  String valStr = "";
  switch(addr) {
    case 0x0200: case 0x0201: case 0x0203: case 0x0209: case 0x020A: case 0x0044: case 0x03E9: {
      float temp = (int16_t)val / 10.0;
      char tempBuf[10];
      dtostrf(temp, 4, 1, tempBuf);
      String tStr = String(tempBuf);
      tStr.trim();
      tStr.replace('.', ',');
      valStr = tStr + " °C";
      break;
    }
    case 0x01FE: {
      float freq = val / 10.0;
      char freqBuf[10];
      dtostrf(freq, 4, 1, freqBuf);
      String fStr = String(freqBuf);
      fStr.trim();
      fStr.replace('.', ',');
      valStr = fStr + " Hz";
      break;
    }
    case 0x03E8: {
      if (val == MODE_OFF) valStr = String(val) + " (AUS)";
      else if (val == MODE_OFF_PRESET) valStr = String(val) + " (AUS PRESET)";
      else if (val == MODE_AUTO) valStr = String(val) + " (AUTO)";
      else if (val == MODE_COOL) valStr = String(val) + " (KÜHLEN)";
      else if (val == MODE_SMART) valStr = String(val) + " (SMART)";
      else if (val == MODE_BOOST) valStr = String(val) + " (BOOST)";
      else if (val == MODE_ECO) valStr = String(val) + " (ECO)";
      else valStr = String(val);
      break;
    }
    default:
      valStr = String(val);
      break;
  }
  return valStr + " (" + String(hexVal) + ")";
}

void flushModbus() {
  while(ModbusSerial.available()) ModbusSerial.read();
}

uint8_t writeRegFC10(uint16_t regAddress, uint16_t value) {
  node.setTransmitBuffer(0, value);
  return node.writeMultipleRegisters(regAddress, 1);
}

// Forward Declarations
void publishAllMQTT();
void updateActiveData();

// --- MQTT Hilfsfunktionen ---
void sendMQTT(const String& subtopic, const String& payload) {
  if (mqttClient.connected()) {
    String fullTopic = String(mqtt_prefix) + "/" + subtopic;
    mqttClient.publish(fullTopic.c_str(), payload.c_str(), true);
  }
}

void publishAllMQTT() {
  if (!mqttClient.connected()) return;
  
  sendMQTT("status", "online");
  sendMQTT("ip", WiFi.localIP().toString());
  sendMQTT("free_heap", String(ESP.getFreeHeap())); // RAM via MQTT
  sendMQTT("power", (pwp.raw_mode == MODE_OFF) ? "OFF" : "ON");
  
  String modeStr = "UNKNOWN";
  if (pwp.raw_mode == MODE_OFF) modeStr = "OFF";
  else if (pwp.raw_mode == MODE_OFF_PRESET) modeStr = "OFF_PRESET";
  else if (pwp.raw_mode == MODE_AUTO) modeStr = "AUTO";
  else if (pwp.raw_mode == MODE_COOL) modeStr = "COOL";
  else if (pwp.raw_mode == MODE_SMART) modeStr = "SMART";
  else if (pwp.raw_mode == MODE_BOOST) modeStr = "BOOST";
  else if (pwp.raw_mode == MODE_ECO) modeStr = "ECO";
  
  sendMQTT("mode", modeStr);
  sendMQTT("mode_raw", String(pwp.raw_mode));

  sendMQTT("target_temp", String(pwp.target_temp, 1));
  sendMQTT("water_in_temp", String(pwp.water_in_temp, 1));
  sendMQTT("water_out_temp", String(pwp.water_out_temp, 1));
  sendMQTT("outdoor_temp", String(pwp.outdoor_temp, 1));
  sendMQTT("hotgas_temp", String(pwp.hotgas_temp, 1));
  sendMQTT("suction_temp", String(pwp.suction_temp, 1));
  sendMQTT("evap_temp", String(pwp.evap_temp, 1));
  sendMQTT("compressor_freq", String(pwp.compressor_freq, 1));
  sendMQTT("fan_status", String(pwp.fan_status));
}

// Speichere Daten in Cache & Sende bei Änderung via MQTT
void updateCacheAndPwp(uint16_t addr, uint16_t val, bool &isChanged) {
  isChanged = false;
  int foundIdx = -1;
  for (int r = 0; r < regCacheCount; r++) {
    if (regCache[r].addr == addr) {
      foundIdx = r;
      break;
    }
  }

  if (foundIdx == -1 && regCacheCount < 128) {
    regCache[regCacheCount] = {addr, val};
    regCacheCount++;
    isChanged = true;
  } else if (foundIdx != -1 && regCache[foundIdx].val != val) {
    regCache[foundIdx].val = val;
    isChanged = true;
  }

  switch(addr) {
    case 0x0200: 
      pwp.water_in_temp = (int16_t)val / 10.0;
      if (isChanged) sendMQTT("water_in_temp", String(pwp.water_in_temp, 1));
      break;
    case 0x0201: 
      pwp.water_out_temp = (int16_t)val / 10.0;
      if (isChanged) sendMQTT("water_out_temp", String(pwp.water_out_temp, 1));
      break;
    case 0x0203: 
      pwp.outdoor_temp = (int16_t)val / 10.0;
      if (isChanged) sendMQTT("outdoor_temp", String(pwp.outdoor_temp, 1));
      break;
    case 0x01FE: 
      pwp.compressor_freq = val / 10.0;
      if (isChanged) sendMQTT("compressor_freq", String(pwp.compressor_freq, 1));
      break;
    case 0x0209: 
      pwp.suction_temp = (int16_t)val / 10.0;
      if (isChanged) sendMQTT("suction_temp", String(pwp.suction_temp, 1));
      break;
    case 0x020A: 
      pwp.evap_temp = (int16_t)val / 10.0;
      if (isChanged) sendMQTT("evap_temp", String(pwp.evap_temp, 1));
      break;
    case 0x0044: 
      pwp.hotgas_temp = (int16_t)val / 10.0;
      if (isChanged) sendMQTT("hotgas_temp", String(pwp.hotgas_temp, 1));
      break;
    case 0x01F4:
      pwp.fan_status = val;
      if (isChanged) sendMQTT("fan_status", String(pwp.fan_status));
      break;
    case 0x03E8: {
      pwp.raw_mode = val;
      if (isChanged) {
        sendMQTT("power", (pwp.raw_mode == MODE_OFF) ? "OFF" : "ON");
        sendMQTT("mode_raw", String(pwp.raw_mode));
        
        String modeStr = "UNKNOWN";
        if (pwp.raw_mode == MODE_OFF) modeStr = "OFF";
        else if (pwp.raw_mode == MODE_OFF_PRESET) modeStr = "OFF_PRESET";
        else if (pwp.raw_mode == MODE_AUTO) modeStr = "AUTO";
        else if (pwp.raw_mode == MODE_COOL) modeStr = "COOL";
        else if (pwp.raw_mode == MODE_SMART) modeStr = "SMART";
        else if (pwp.raw_mode == MODE_BOOST) modeStr = "BOOST";
        else if (pwp.raw_mode == MODE_ECO) modeStr = "ECO";
        sendMQTT("mode", modeStr);
      }
      break;
    }
    case 0x03E9: 
      pwp.target_temp = (int16_t)val / 10.0;
      if (isChanged) sendMQTT("target_temp", String(pwp.target_temp, 1));
      break;
  }
}

// --- PASSIVER MODBUS TUYA SNIFFER ---
#define FRAME_TIMEOUT 3 
uint8_t snifferFrame[1024];
uint16_t frameLen = 0;
unsigned long lastByteTime = 0;
uint16_t pendingReqAddr = 0xFFFF;
String pendingReqStr = "";
bool deltaOnlyMode = false;

void processSnifferFrame(uint8_t* buf, uint16_t len) {
  addLog("[RAW] " + bufferToHex(buf, len));

  if (buf[0] == 0x11 && buf[1] == 0x10 && len >= 9) {
    uint16_t writeAddr = (buf[2] << 8) | buf[3];
    uint16_t writeVal  = (buf[7] << 8) | buf[8];
    bool dummy;
    updateCacheAndPwp(writeAddr, writeVal, dummy);
    
    addLog("[SNIFF] ✍️ WRITE FC10 | Reg " + hex4(writeAddr) + " (" + getRegisterName(writeAddr) + ") = " + decodeRegValue(writeAddr, writeVal));
    pendingReqAddr = 0xFFFF;
    return;
  }

  if (buf[0] == 0x11 && buf[1] == 0x03 && len == 8) {
    if (pendingReqAddr != 0xFFFF && !deltaOnlyMode) {
      addLog("[SNIFF] " + pendingReqStr + " Reg " + hex4(pendingReqAddr) + " (" + getRegisterName(pendingReqAddr) + ") [KEINE ANTWORT]");
    }
    pendingReqAddr = (buf[2] << 8) | buf[3];
    pendingReqStr = formatReqFC03(buf);
    return;
  }
  
  if (buf[0] == 0x11 && buf[1] == 0x03 && len >= 5) {
    uint8_t byteCount = buf[2];
    uint16_t startAddr = (pendingReqAddr != 0xFFFF) ? pendingReqAddr : 0x0000;
    
    bool anyChanged = false;
    String decodeLogStr = "";

    for (int i = 0; i < byteCount; i += 2) {
      if ((3 + i + 1) >= len) break;
      uint16_t currentAddr = startAddr + (i / 2);
      uint16_t currentVal = (buf[3 + i] << 8) | buf[4 + i];
      
      bool valChanged = false;
      updateCacheAndPwp(currentAddr, currentVal, valChanged);
      if (valChanged) anyChanged = true;

      if (i > 0) decodeLogStr += ", ";
      decodeLogStr += "Reg " + hex4(currentAddr) + " = " + decodeRegValue(currentAddr, currentVal);
    }

    if (anyChanged || !deltaOnlyMode) {
      String reqPart = (pendingReqAddr != 0xFFFF) ? pendingReqStr : "11|03|?? ??|?? ??|?? ??";
      String regName = (pendingReqAddr != 0xFFFF) ? getRegisterName(pendingReqAddr) : "Unbekannt";
      addLog("[SNIFF] " + reqPart + " Reg " + hex4(startAddr) + " (" + regName + ") -> " + formatRespFC03(buf, len) + " - " + decodeLogStr);
    }
    
    pendingReqAddr = 0xFFFF;
    return;
  }

  if (!deltaOnlyMode && buf[0] != 0x11) {
    addLog("[SNIFF] Fremdes Paket / Broadcast / Unbekannt");
  }
}

void handleSniffer() {
  while (ModbusSerial.available()) {
    if (frameLen < 1024) {
      snifferFrame[frameLen++] = ModbusSerial.read();
    } else {
      ModbusSerial.read();
    }
    lastByteTime = millis();
  }
  
  if (frameLen > 0 && (millis() - lastByteTime >= FRAME_TIMEOUT)) {
    processSnifferFrame(snifferFrame, frameLen);
    frameLen = 0;
  }
}

// --- Steuerungs-Funktionen ---
void setTargetTemp(float newTemp) {
  if (newTemp < 15.0) newTemp = 15.0;
  if (newTemp > 35.0) newTemp = 35.0;
  
  uint16_t modbusVal = (uint16_t)(newTemp * 10.0);
  flushModbus();
  uint8_t res = writeRegFC10(0x03E9, modbusVal);
  if (res == node.ku8MBSuccess) {
    pwp.target_temp = newTemp;
    addLog("FC10 SUCCESS: Soll-Temp (0x03E9) -> " + String(newTemp, 1) + "°C");
    publishAllMQTT();
  } else {
    addLog("FC10 ERROR Temp: Code 0x" + String(res, HEX));
  }
}

void setHeatPumpMode(uint16_t newMode) {
  uint16_t targetTempRaw = (uint16_t)(pwp.target_temp * 10.0);
  flushModbus();

  writeRegFC10(0x03E9, targetTempRaw);
  delay(50);

  if (newMode == MODE_OFF) {
    writeRegFC10(0x03E8, MODE_OFF_PRESET);
    delay(50);
    
    uint8_t res = writeRegFC10(0x03E8, MODE_OFF);
    if (res == node.ku8MBSuccess) {
      pwp.raw_mode = MODE_OFF;
      addLog("FC10 SUCCESS: Modus -> AUS (0)");
      publishAllMQTT();
    } else {
      addLog("FC10 ERROR (OFF): Code 0x" + String(res, HEX));
    }
  } else {
    writeRegFC10(0x03E8, MODE_COOL);
    delay(50);
    
    uint8_t res = writeRegFC10(0x03E8, newMode);
    if (res == node.ku8MBSuccess) {
      pwp.raw_mode = newMode;
      addLog("FC10 SUCCESS: Modus -> " + decodeRegValue(0x03E8, newMode));
      publishAllMQTT();
    } else {
      addLog("FC10 ERROR (" + String(newMode) + "): Code 0x" + String(res, HEX));
    }
  }
}

// --- MQTT Empfangs-Callback ---
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
  message.trim();
  String topStr = String(topic);

  addLog("📥 MQTT Empfangen [" + topStr + "]: " + message);

  String setPrefix = String(mqtt_prefix) + "/set/";
  
  if (topStr == setPrefix + "mode" || topStr == String(mqtt_prefix) + "/mode/set") {
    message.toUpperCase();
    uint16_t newMode = MODE_OFF;
    if (message == "OFF" || message == "0") newMode = MODE_OFF;
    else if (message == "AUTO" || message == "17") newMode = MODE_AUTO;
    else if (message == "COOL" || message == "18") newMode = MODE_COOL;
    else if (message == "SMART" || message == "21") newMode = MODE_SMART;
    else if (message == "BOOST" || message == "22") newMode = MODE_BOOST;
    else if (message == "ECO" || message == "23") newMode = MODE_ECO;
    else newMode = message.toInt();
    
    setHeatPumpMode(newMode);
  }
  else if (topStr == setPrefix + "target_temp" || topStr == String(mqtt_prefix) + "/target_temp/set") {
    float temp = message.toFloat();
    if (temp >= 15.0 && temp <= 35.0) {
      setTargetTemp(temp);
    } else {
      addLog("⚠️ Ungültiger Temp-Wert via MQTT: " + message);
    }
  }
  else if (topStr == setPrefix + "power" || topStr == String(mqtt_prefix) + "/power/set") {
    message.toUpperCase();
    if (message == "OFF" || message == "0" || message == "FALSE") {
      setHeatPumpMode(MODE_OFF);
    } else if (message == "ON" || message == "1" || message == "TRUE") {
      if (pwp.raw_mode == MODE_OFF) setHeatPumpMode(MODE_SMART);
    }
  }
  else if (topStr == setPrefix + "poll" || topStr == String(mqtt_prefix) + "/poll/set" || topStr == setPrefix + "refresh") {
    updateActiveData();
  }
}

void reconnectMQTT() {
  static unsigned long lastMQTTAttempt = 0;
  if (!mqttClient.connected()) {
    unsigned long now = millis();
    if (now - lastMQTTAttempt > 5000) {
      lastMQTTAttempt = now;
      bool connected = false;
      String lwtTopic = String(mqtt_prefix) + "/status";

      if (strlen(mqtt_user) > 0) {
        connected = mqttClient.connect(mqtt_client_id, mqtt_user, mqtt_pass, lwtTopic.c_str(), 1, true, "offline");
      } else {
        connected = mqttClient.connect(mqtt_client_id, lwtTopic.c_str(), 1, true, "offline");
      }

      if (connected) {
        addLog("✅ MQTT Verbunden als '" + String(mqtt_client_id) + "'");
        mqttClient.setCallback(mqttCallback);
        
        String setPrefix = String(mqtt_prefix) + "/set/";
        mqttClient.subscribe((setPrefix + "#").c_str());
        mqttClient.subscribe((String(mqtt_prefix) + "/+/set").c_str());

        publishAllMQTT();
      } else {
        addLog("❌ MQTT Verbindung fehlgeschlagen. Status=" + String(mqttClient.state()));
      }
    }
  }
}

// Aktiver Master-Poll (Fallback)
bool readSingleRegisterActive(uint16_t regAddr) {
  flushModbus();
  delay(30);
  uint8_t result = node.readHoldingRegisters(regAddr, 1);
  if (result == node.ku8MBSuccess) {
    uint16_t val = node.getResponseBuffer(0);
    bool dummy;
    updateCacheAndPwp(regAddr, val, dummy);
    return true;
  } else {
    addLog("⚠️ Abfragefehler bei Reg " + hex4(regAddr) + " | Code: 0x" + String(result, HEX));
    return false;
  }
}

void updateActiveData() {
  const uint16_t regsToPoll[] = {
    0x0203, 0x03E9, 0x03E8, 0x0200, 0x0201, 0x0044, 0x0209, 0x020A, 0x01FE, 0x01F4
  };

  int successCount = 0;
  int totalRegs = sizeof(regsToPoll) / sizeof(regsToPoll[0]);

  addLog("🔄 Starte manuelle Modbus-Abfrage...");
  for (int i = 0; i < totalRegs; i++) {
    if (readSingleRegisterActive(regsToPoll[i])) {
      successCount++;
    }
  }

  if (successCount > 0) {
    addLog("✅ Manuelle Abfrage OK: " + String(successCount) + "/" + String(totalRegs) + " Register gelesen.");
    publishAllMQTT();
  }
}

// --- HTTP Reset Endpoint (/reset) ---
void handleResetEndpoint() {
  addLog(">>> ESP32 REBOOT AUSGEFÜHRT VIA /reset <<<");
  server.send(200, "text/html", "<html><head><meta charset='utf-8'></head><body style='font-family:sans-serif; text-align:center; padding-top:50px;'><h3>ESP32 wird neu gestartet (/reset)...</h3><p>Automatische Weiterleitung in 8 Sekunden.</p><script>setTimeout(function(){ window.location.href='/'; }, 8000);</script></body></html>");
  delay(1000);
  ESP.restart();
}

// --- Webserver Endpunkte ---
void handleLogs() {
  String out = "";
  for(int i = 0; i < MAX_LOG_LINES; i++) {
    int idx = (logIndex + i) % MAX_LOG_LINES;
    if(logBuffer[idx].length() > 0) out += logBuffer[idx] + "\n";
  }
  server.send(200, "text/plain", out);
}

void handleRoot() {
  if (server.hasArg("refresh")) {
    updateActiveData();
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }

  if (server.hasArg("reboot")) {
    handleResetEndpoint();
    return;
  }

  if (server.hasArg("setMode")) {
    uint16_t newMode = server.arg("setMode").toInt();
    setHeatPumpMode(newMode);
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }

  if (server.hasArg("setTemp")) {
    float newTemp = server.arg("setTemp").toFloat();
    setTargetTemp(newTemp);
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }

  // Multi-UI HTML (Dashboard, Mobile UI, Diagnostics UI)
  String html = "<!DOCTYPE html><html lang='de'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>BWT Heatpump Controller</title>";
  html += "<style>";
  html += ":root { --primary:#0275d8; --bg:#f4f6f9; --card:#ffffff; --text:#333; --border:#e0e0e0; }";
  html += "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: var(--bg); color: var(--text); margin:0; padding:15px; }";
  html += ".container { max-width:1100px; margin:0 auto; }";
  html += ".header { background:#1e293b; color:white; padding:15px 20px; border-radius:10px; display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:10px; margin-bottom:15px; }";
  html += ".header h1 { margin:0; font-size:1.4rem; }";
  html += ".badge { background:#10b981; color:white; padding:4px 8px; border-radius:4px; font-size:0.8rem; font-weight:bold; }";
  html += ".badge-ram { background:#8b5cf6; color:white; padding:4px 8px; border-radius:4px; font-size:0.8rem; font-weight:bold; }";
  html += ".nav-tabs { display:flex; gap:8px; margin-bottom:15px; border-bottom:2px solid var(--border); padding-bottom:8px; }";
  html += ".tab-btn { padding:10px 18px; border:none; background:#e2e8f0; color:#475569; font-weight:bold; border-radius:6px; cursor:pointer; font-size:0.95rem; }";
  html += ".tab-btn.active { background:var(--primary); color:white; }";
  html += ".tab-content { display:none; }";
  html += ".tab-content.active { display:block; }";
  html += ".card { background:var(--card); border-radius:10px; padding:20px; border:1px solid var(--border); box-shadow:0 2px 8px rgba(0,0,0,0.05); margin-bottom:15px; }";
  html += ".grid { display:grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); gap:15px; }";
  html += ".btn { display:inline-block; padding:10px 16px; border-radius:6px; font-weight:bold; text-decoration:none; color:white; border:none; cursor:pointer; text-align:center; }";
  html += ".btn-poll { background:#0275d8; } .btn-danger { background:#ef4444; } .btn-off { background:#64748b; } .btn-auto { background:#0275d8; } .btn-cool { background:#06b6d4; } .btn-smart { background:#f59e0b; } .btn-boost { background:#8b5cf6; } .btn-eco { background:#10b981; }";
  html += ".mode-grid { display:grid; grid-template-columns: repeat(auto-fit, minmax(100px, 1fr)); gap:8px; margin-top:10px; }";
  html += ".mobile-box { max-width:420px; margin:0 auto; background:white; padding:20px; border-radius:16px; box-shadow:0 4px 16px rgba(0,0,0,0.1); }";
  html += ".temp-control { display:flex; align-items:center; justify-content:center; gap:20px; margin:20px 0; }";
  html += ".temp-btn { width:50px; height:50px; border-radius:50%; font-size:1.8rem; background:#e2e8f0; color:#1e293b; border:none; cursor:pointer; font-weight:bold; display:flex; align-items:center; justify-content:center; text-decoration:none; }";
  html += ".temp-val { font-size:2.2rem; font-weight:bold; }";
  html += "table { width:100%; border-collapse:collapse; margin-top:10px; } th, td { padding:10px; border-bottom:1px solid var(--border); text-align:left; } th { background:#f8fafc; }";
  html += "textarea { width:100%; height:320px; background:#0f172a; color:#22c55e; font-family:monospace; padding:12px; border-radius:8px; border:none; resize:vertical; box-sizing:border-box; }";
  html += "select { padding:6px 12px; border-radius:6px; border:1px solid var(--border); font-weight:bold; }";
  html += "</style></head><body>";

  html += "<div class='container'>";
  
  html += "<div class='header'>";
  html += "<div><h1>BWT Heatpump Controller</h1><small>Mode: SNIFFER & MASTER | IP: " + WiFi.localIP().toString() + "</small></div>";
  html += "<div><span class='badge-ram'>RAM: " + String(ESP.getFreeHeap() / 1024) + " KB free</span> ";
  html += "<span class='badge'>MQTT: " + String(mqttClient.connected() ? "CONNECTED" : "DISCONNECTED") + "</span> ";
  html += "<a href='/?refresh=1' class='btn btn-poll'>🔄 Manual Poll</a></div>";
  html += "</div>";

  html += "<div class='nav-tabs'>";
  html += "<button class='tab-btn' onclick='openTab(\"dash\")'>📊 Dashboard (Desktop UI)</button>";
  html += "<button class='tab-btn' onclick='openTab(\"mobile\")'>📱 Mobile Remote (Phone UI)</button>";
  html += "<button class='tab-btn' onclick='openTab(\"diag\")'>🛠️ Diagnostic Tool (Advanced)</button>";
  html += "</div>";

  // --- TAB 1: DESKTOP DASHBOARD UI ---
  html += "<div id='dash' class='tab-content'>";
  html += "<div class='grid'>";
  
  html += "<div class='card'>";
  html += "<h3>Betriebsmodus</h3>";
  html += "<p>Aktuell: <b>" + decodeRegValue(0x03E8, pwp.raw_mode) + "</b></p>";
  html += "<div class='mode-grid'>";
  html += "<a href='/?setMode=" + String(MODE_OFF) + "' class='btn btn-off'>OFF (0)</a>";
  html += "<a href='/?setMode=" + String(MODE_AUTO) + "' class='btn btn-auto'>AUTO (17)</a>";
  html += "<a href='/?setMode=" + String(MODE_COOL) + "' class='btn btn-cool'>COOL (18)</a>";
  html += "<a href='/?setMode=" + String(MODE_SMART) + "' class='btn btn-smart'>SMART (21)</a>";
  html += "<a href='/?setMode=" + String(MODE_BOOST) + "' class='btn btn-boost'>BOOST (22)</a>";
  html += "<a href='/?setMode=" + String(MODE_ECO) + "' class='btn btn-eco'>ECO (23)</a>";
  html += "</div>";
  
  html += "<h3 style='margin-top:20px;'>Soll-Temperatur (0x03E9)</h3>";
  html += "<form action='/' method='GET' style='display:flex; align-items:center; gap:10px;'>";
  html += "<input type='range' min='15' max='35' step='0.5' name='setTemp' value='" + String(pwp.target_temp, 1) + "' oninput='this.nextElementSibling.value = this.value + \" °C\"'>";
  html += "<output style='font-size:1.2rem; font-weight:bold; min-width:60px;'>" + String(pwp.target_temp, 1) + " °C</output>";
  html += "<input type='submit' class='btn btn-poll' value='Set'>";
  html += "</form>";
  html += "</div>";

  html += "<div class='card'>";
  html += "<h3>Sensor-Übersicht</h3>";
  html += "<div style='display:grid; grid-template-columns:1fr 1fr; gap:10px;'>";
  html += "<p>🌡️ <b>Außentemp:</b><br>" + String(pwp.outdoor_temp, 1) + " °C</p>";
  html += "<p>🎯 <b>Soll-Temp:</b><br>" + String(pwp.target_temp, 1) + " °C</p>";
  html += "<p>💧 <b>Vorlauf-Temp:</b><br>" + String(pwp.water_in_temp, 1) + " °C</p>";
  html += "<p>💧 <b>Rücklauf-Temp:</b><br>" + String(pwp.water_out_temp, 1) + " °C</p>";
  html += "<p>🔥 <b>Heißgas-Temp:</b><br>" + String(pwp.hotgas_temp, 1) + " °C</p>";
  html += "<p>❄️ <b>Verdampfer-Temp:</b><br>" + String(pwp.evap_temp, 1) + " °C</p>";
  html += "<p>⚡ <b>Kompressor:</b><br>" + String(pwp.compressor_freq, 1) + " Hz</p>";
  html += "<p>🌀 <b>Status/Lüfter:</b><br>" + String(pwp.fan_status) + "</p>";
  html += "</div></div>";

  html += "</div></div>";

  // --- TAB 2: MOBILE REMOTE UI ---
  html += "<div id='mobile' class='tab-content'>";
  html += "<div class='mobile-box'>";
  html += "<div style='text-align:center;'>";
  html += "<small style='text-transform:uppercase; color:#64748b; font-weight:bold;'>BWT Remote Master</small>";
  html += "<h2 style='margin:5px 0;'>Modus: " + decodeRegValue(0x03E8, pwp.raw_mode) + "</h2>";
  
  html += "<div class='temp-control'>";
  html += "<a href='/?setTemp=" + String(pwp.target_temp - 0.5, 1) + "' class='temp-btn'>-</a>";
  html += "<span class='temp-val'>" + String(pwp.target_temp, 1) + " °C</span>";
  html += "<a href='/?setTemp=" + String(pwp.target_temp + 0.5, 1) + "' class='temp-btn'>+</a>";
  html += "</div>";

  html += "<div class='mode-grid' style='margin-bottom:20px;'>";
  html += "<a href='/?setMode=" + String(MODE_OFF) + "' class='btn btn-off'>OFF</a>";
  html += "<a href='/?setMode=" + String(MODE_SMART) + "' class='btn btn-smart'>SMART</a>";
  html += "<a href='/?setMode=" + String(MODE_BOOST) + "' class='btn btn-boost'>BOOST</a>";
  html += "<a href='/?setMode=" + String(MODE_ECO) + "' class='btn btn-eco'>ECO</a>";
  html += "</div>";

  html += "<div style='text-align:left; border-top:1px solid #e2e8f0; padding-top:10px;'>";
  html += "<p>🌡️ <b>Außen:</b> " + String(pwp.outdoor_temp, 1) + " °C</p>";
  html += "<p>💧 <b>Vorlauf:</b> " + String(pwp.water_in_temp, 1) + " °C</p>";
  html += "<p>💧 <b>Rücklauf:</b> " + String(pwp.water_out_temp, 1) + " °C</p>";
  html += "<p>🔥 <b>Heißgas:</b> " + String(pwp.hotgas_temp, 1) + " °C</p>";
  html += "<p>⚡ <b>Kompressor:</b> " + String(pwp.compressor_freq, 1) + " Hz</p>";
  html += "</div>";

  html += "<a href='/?refresh=1' class='btn btn-poll' style='width:100%; box-sizing:border-box; margin-top:15px;'>Read Values Now</a>";
  html += "</div></div></div>";

  // --- TAB 3: ADVANCED DIAGNOSTICS UI ---
  html += "<div id='diag' class='tab-content'>";
  html += "<div class='card'>";
  html += "<h3>System & Speicher Status</h3>";
  html += "<p>🧠 <b>Freier RAM (Heap):</b> " + String(ESP.getFreeHeap() / 1024.0, 2) + " KB (" + String(ESP.getFreeHeap()) + " Bytes)</p>";
  html += "<p>📉 <b>Minimaler freier RAM:</b> " + String(ESP.getMinFreeHeap() / 1024.0, 2) + " KB</p>";
  html += "<p>📦 <b>Gesamt Heap-Größe:</b> " + String(ESP.getHeapSize() / 1024.0, 2) + " KB</p>";
  
  html += "<div style='margin-top:15px; display:flex; gap:10px; flex-wrap:wrap;'>";
  html += "<a href='/update' class='btn btn-poll'>📁 Firmware Web-OTA Upload</a>";
  html += "<a href='/reset' onclick='return confirm(\"ESP32 wirklich via /reset neu starten?\");' class='btn btn-danger'>ESP32 Reboot (/reset)</a>";
  html += "</div></div>";

  html += "<div class='card'>";
  html += "<h3>Register-Tabelle</h3>";
  
  const uint16_t displayRegs[] = {
    0x0203, 0x03E9, 0x03E8, 0x0200, 0x0201, 0x0044, 0x0209, 0x020A, 0x01FE, 0x01F4
  };
  int totalDisplay = sizeof(displayRegs) / sizeof(displayRegs[0]);

  html += "<table><tr><th>Register (Hex / Dez)</th><th>Bezeichnung</th><th>Aktueller Wert (Dekodiert)</th></tr>";
  for (int i = 0; i < totalDisplay; i++) {
    uint16_t a = displayRegs[i];
    uint16_t v = 0;
    bool found = false;
    for (int r = 0; r < regCacheCount; r++) {
      if (regCache[r].addr == a) { v = regCache[r].val; found = true; break; }
    }
    String valDisplay = found ? decodeRegValue(a, v) : "--- (Nicht gelesen)";
    html += "<tr><td>" + hex4(a) + " (" + String(a) + ")</td><td>" + getRegisterName(a) + "</td><td><b>" + valDisplay + "</b></td></tr>";
  }
  html += "</table></div>";

  html += "<div class='card'>";
  html += "<div style='display:flex; justify-content:space-between; align-items:center; margin-bottom:10px; flex-wrap:wrap; gap:10px;'>";
  html += "<h3 style='margin:0;'>Live Console Logs</h3>";
  html += "<div><label for='logFilterSelect'><b>Format: </b></label>";
  html += "<select id='logFilterSelect' onchange='setLogFilter(this.value)'>";
  html += "<option value='ALL'>Alle Logs (RAW & Human)</option>";
  html += "<option value='HUMAN'>Nur Human Readable (Lesbar)</option>";
  html += "<option value='RAW'>Nur RAW Hex-Frames</option>";
  html += "</select></div>";
  html += "</div>";
  html += "<textarea id='logbox' readonly></textarea>";
  html += "</div></div>";

  html += "</div>";

  // JavaScript
  html += "<script>";
  html += "let rawLogData = '';";
  html += "let currentFilter = localStorage.getItem('logFilter') || 'ALL';";

  html += "function openTab(tabId) {";
  html += "  document.querySelectorAll('.tab-content').forEach(c => c.classList.remove('active'));";
  html += "  document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));";
  html += "  var t = document.getElementById(tabId);";
  html += "  if(t) t.classList.add('active');";
  html += "  var btn = document.querySelector('button[onclick=\"openTab(\\\'' + tabId + '\\\')\"]');";
  html += "  if(btn) btn.classList.add('active');";
  html += "  localStorage.setItem('activeTab', tabId);";
  html += "}";

  html += "function setLogFilter(mode) {";
  html += "  currentFilter = mode;";
  html += "  localStorage.setItem('logFilter', mode);";
  html += "  applyLogFilter();";
  html += "}";

  html += "function applyLogFilter() {";
  html += "  let b = document.getElementById('logbox');";
  html += "  if (!b) return;";
  html += "  let lines = rawLogData.split('\\n');";
  html += "  let filtered = lines.filter(line => {";
  html += "    if (currentFilter === 'RAW') return line.includes('[RAW]');";
  html += "    if (currentFilter === 'HUMAN') return !line.includes('[RAW]');";
  html += "    return true;";
  html += "  });";
  html += "  b.value = filtered.join('\\n');";
  html += "  b.scrollTop = b.scrollHeight;";
  html += "}";

  html += "setInterval(function() {";
  html += "  fetch('/logdata').then(r => r.text()).then(d => {";
  html += "    if (rawLogData !== d) {";
  html += "      rawLogData = d;";
  html += "      applyLogFilter();";
  html += "    }";
  html += "  });";
  html += "}, 1200);";

  html += "window.onload = function() {";
  html += "  let savedTab = localStorage.getItem('activeTab') || 'dash';";
  html += "  openTab(savedTab);";
  html += "  let savedFilter = localStorage.getItem('logFilter') || 'ALL';";
  html += "  let sel = document.getElementById('logFilterSelect');";
  html += "  if (sel) sel.value = savedFilter;";
  html += "  currentFilter = savedFilter;";
  html += "};";
  html += "</script></body></html>";

  server.send(200, "text/html", html);
}

void setup() {
  Serial.begin(115200);
  ModbusSerial.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);
  node.begin(17, ModbusSerial);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); }

  // Automatische Zeitsynchronisation für Deutschland (inkl. Sommer-/Winterzeit-Umschaltung)
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov");

  mqttClient.setServer(mqtt_server, mqtt_port);

  // 1. ArduinoOTA (IDE Flash)
  ArduinoOTA.setHostname("bwt-heatpump-esp32");
  ArduinoOTA.setPassword("11111111");
  ArduinoOTA.begin();

// 2. HTTP Web-OTA Upload (/update) - Eigene Implementierung ohne FileSystem
  server.on("/update", HTTP_GET, []() {
    String html = "<!DOCTYPE html><html lang='de'><head><meta charset='utf-8'>";
    html += "<style>body{font-family:sans-serif;padding:20px;background:#f4f6f9;color:#333;}</style></head><body>";
    html += "<h2>BWT Heatpump - Firmware Update (.bin)</h2>";
    html += "<p>Wähle deine kompilierte Firmware-Datei aus:</p>";
    html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
    html += "<input type='file' name='update' accept='.bin' required><br><br>";
    html += "<input type='submit' value='Firmware hochladen' style='padding:8px 16px; background:#0275d8; color:white; border:none; border-radius:4px; cursor:pointer;'>";
    html += "</form>";
    html += "<br><a href='/'>&larr; Zurück zum Dashboard</a>";
    html += "</body></html>";
    server.send(200, "text/html", html);
  });

  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "Update fehlgeschlagen!" : "Update erfolgreich! ESP32 startet neu...");
    delay(1000);
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      addLog("📦 OTA-Start: " + String(upload.filename.c_str()));
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) { // Erzwangenes Firmware-Flash (U_FLASH)
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        addLog("✅ OTA-Update erfolgreich: " + String(upload.totalSize) + " Bytes");
      } else {
        Update.printError(Serial);
      }
    }
  });

  // 3. Webserver Endpunkte
  server.on("/", handleRoot);
  server.on("/reset", handleResetEndpoint); // Reset-Endpunkt: http://x.x.x.x/reset
  server.on("/logdata", handleLogs); 
  server.begin();
  
  addLog("System gestartet (Sniffer + Hybrid Master). IP: " + WiFi.localIP().toString());
  addLog("Freier Heap: " + String(ESP.getFreeHeap() / 1024.0, 1) + " KB");
}

unsigned long lastMQTTPeriodicUpdate = 0;

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  
  reconnectMQTT();
  mqttClient.loop();

  // Passiver Sniffer Aufruf
  handleSniffer();

  // Periodische MQTT-Sendelogik alle 5 Minuten
  if (millis() - lastMQTTPeriodicUpdate > 300000) {
    publishAllMQTT();
    lastMQTTPeriodicUpdate = millis();
  }
}
