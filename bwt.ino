#include <WiFi.h>
#include <WebServer.h>
#include <ModbusMaster.h>
#include <ArduinoOTA.h>

// --- WLAN Konfiguration ---
const char* ssid = "DEIN_WLAN_NAME";
const char* password = "DEIN_WLAN_PASSWORT";

// --- Hardware Pins (ESP32-C3) ---
#define RX_PIN 2
#define TX_PIN 3

// --- Betriebsmodi der Wärmepumpe (Register 0x03E8) ---
enum HeatPumpMode {
    MODE_OFF   = 0,
    MODE_ON    = 1,
    MODE_SMART = 21,
    MODE_BOOST = 22,
    MODE_ECO   = 23
};

WebServer server(80);
ModbusMaster node;
HardwareSerial ModbusSerial(1);

// --- Betriebsmodus ---
bool isSnifferMode = true;
bool deltaOnlyMode = true;

// --- Web-Logger (100 Zeilen Ringpuffer) ---
const int MAX_LOG_LINES = 100;
String logBuffer[MAX_LOG_LINES];
int logIndex = 0;

void addLog(String msg) {
  Serial.println(msg); 
  logBuffer[logIndex] = msg;
  logIndex = (logIndex + 1) % MAX_LOG_LINES;
}

// --- Dynamic Register Cache ---
struct RegisterState {
  uint16_t addr;
  uint16_t val;
};
RegisterState regCache[128];
int regCacheCount = 0;

// --- Request/Response Pairing für Sniffer ---
uint16_t pendingReqAddr = 0xFFFF;
String pendingReqStr = "";

// --- Active Mode & Live-Anzeige Datenstruktur ---
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
  uint16_t status_code = 0;
} pwp;

// --- Formatierungs-Hilfsfunktionen ---
String hex2(uint8_t b) {
  char tmp[3];
  sprintf(tmp, "%02x", b);
  return String(tmp);
}

String hex4(uint16_t v) {
  char tmp[7];
  sprintf(tmp, "0x%04X", v);
  return String(tmp);
}

String getRegisterName(uint16_t addr) {
  switch(addr) {
    case 0x0200: return "Vorlauf-Temp";
    case 0x0201: return "Rücklauf-Temp";
    case 0x0203: return "Außentemp";
    case 0x01FE: return "Kompressor-Freq";
    case 0x0209: return "Saugrohr-Temp";
    case 0x020A: return "Verdampfer-Temp";
    case 0x0044: return "Heißgas-Temp";
    case 0x01FB: return "Status-Code";
    case 0x03E8: return "Betriebsmodus";
    case 0x03E9: return "Soll-Temp";
    case 0x01F4: return "Status/Lüfter (0x01F4)";
    case 0x01F5: return "Sensor (0x01F5)";
    case 0x01F7: return "Sensor (0x01F7)";
    case 0x003B: return "Verdichter (0x003B)";
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
      else if (val == MODE_ON) valStr = String(val) + " (EIN)";
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

String formatReqFC03(uint8_t* buf) {
  return hex2(buf[0]) + "|" + hex2(buf[1]) + "|" + hex2(buf[2]) + " " + hex2(buf[3]) + "|" + hex2(buf[4]) + " " + hex2(buf[5]) + "|" + hex2(buf[6]) + " " + hex2(buf[7]);
}

String formatRespFC03(uint8_t* buf, uint16_t len) {
  String out = hex2(buf[0]) + "|" + hex2(buf[1]) + "|" + hex2(buf[2]) + "|";
  for (int i = 3; i < len - 2; i += 2) {
    if (i > 3) out += " ";
    out += hex2(buf[i]) + " " + hex2(buf[i+1]);
  }
  out += "|" + hex2(buf[len-2]) + " " + hex2(buf[len-1]);
  return out;
}

void flushModbus() {
  while(ModbusSerial.available()) ModbusSerial.read();
}

// Map Sensor Data & Store into dynamic cache
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
    case 0x0200: pwp.water_in_temp = (int16_t)val / 10.0; break;
    case 0x0201: pwp.water_out_temp = (int16_t)val / 10.0; break;
    case 0x0203: pwp.outdoor_temp = (int16_t)val / 10.0; break;
    case 0x01FE: pwp.compressor_freq = val / 10.0; break;
    case 0x0209: pwp.suction_temp = (int16_t)val / 10.0; break;
    case 0x020A: pwp.evap_temp = (int16_t)val / 10.0; break;
    case 0x0044: pwp.hotgas_temp = (int16_t)val / 10.0; break;
    case 0x01FB: pwp.status_code = val; break;
    case 0x03E8: pwp.raw_mode = val; break;
    case 0x03E9: pwp.target_temp = (int16_t)val / 10.0; break;
  }
}

// --- Sniffer Engine mit 6 ms Inter-Byte-Timeout ---
#define FRAME_TIMEOUT 6 
uint8_t snifferFrame[256];
uint16_t frameLen = 0;
unsigned long lastByteTime = 0;

uint8_t writeRegFC10(uint16_t regAddress, uint16_t value) {
  node.setTransmitBuffer(0, value);
  return node.writeMultipleRegisters(regAddress, 1);
}

void processSnifferFrame(uint8_t* buf, uint16_t len) {
  // 1. FC 0x10 (Write)
  if (buf[0] == 0x11 && buf[1] == 0x10 && len >= 9) {
    uint16_t writeAddr = (buf[2] << 8) | buf[3];
    uint16_t writeVal  = (buf[7] << 8) | buf[8];
    bool dummy;
    updateCacheAndPwp(writeAddr, writeVal, dummy);
    
    addLog("✍️ WRITE FC10 | Reg " + hex4(writeAddr) + " (" + getRegisterName(writeAddr) + ") = " + decodeRegValue(writeAddr, writeVal));
    pendingReqAddr = 0xFFFF;
    return;
  }

  // 2. FC 0x03 Request (8 Bytes)
  if (buf[0] == 0x11 && buf[1] == 0x03 && len == 8) {
    if (pendingReqAddr != 0xFFFF && !deltaOnlyMode) {
      addLog(pendingReqStr + " Reg " + hex4(pendingReqAddr) + " (" + getRegisterName(pendingReqAddr) + ") [KEINE ANTWORT]");
    }
    pendingReqAddr = (buf[2] << 8) | buf[3];
    pendingReqStr = formatReqFC03(buf);
    return;
  }
  
  // 3. FC 0x03 Response
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
      addLog(reqPart + " Reg " + hex4(startAddr) + " (" + regName + ") -> " + formatRespFC03(buf, len) + " - " + decodeLogStr);
    }
    
    pendingReqAddr = 0xFFFF;
    return;
  }

  if (!deltaOnlyMode) {
    String raw = "RAW UNKNOWN: ";
    for(int i = 0; i < len; i++) raw += hex2(buf[i]) + " ";
    addLog(raw);
  }
}

void handleSniffer() {
  while (ModbusSerial.available()) {
    snifferFrame[frameLen++] = ModbusSerial.read();
    lastByteTime = millis();
    if (frameLen >= 256) frameLen = 255;
  }
  
  if (frameLen > 0 && (millis() - lastByteTime >= FRAME_TIMEOUT)) {
    processSnifferFrame(snifferFrame, frameLen);
    frameLen = 0;
  }
}

// --- Active Master Logik (Abfrage einzelner Register wie das Tuya Modul) ---
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
    addLog("⚠️ Active Poll Fehler bei Reg " + hex4(regAddr) + " | Code: 0x" + String(result, HEX));
    return false;
  }
}

void updateActiveData() {
  const uint16_t regsToPoll[] = {
    0x0200, // Vorlauf
    0x0201, // Rücklauf
    0x0203, // Außentemp
    0x03E8, // Modus
    0x03E9, // Soll-Temp
    0x01FE, // Kompressor Frequenz
    0x0209, // Saugrohr
    0x020A, // Verdampfer
    0x0044, // Heißgas
    0x01FB, // Status-Code
    0x01F4, // Lüfter / Status
    0x01F5, // Status
    0x01F7, // Status
    0x003B  // Verdichter Status
  };

  int successCount = 0;
  int totalRegs = sizeof(regsToPoll) / sizeof(regsToPoll[0]);

  for (int i = 0; i < totalRegs; i++) {
    if (readSingleRegisterActive(regsToPoll[i])) {
      successCount++;
    }
  }

  if (successCount > 0) {
    addLog("🔄 Active Poll OK: " + String(successCount) + "/" + String(totalRegs) + " Register gelesen.");
  }
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
  if (server.hasArg("mode")) {
    String m = server.arg("mode");
    isSnifferMode = (m == "sniffer");
    addLog(isSnifferMode ? "=== MODUS WECHSEL: SNIFFER (Passiv) ===" : "=== MODUS WECHSEL: ACTIVE MASTER (ID 17) ===");
    flushModbus();
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }

  if (server.hasArg("toggleDelta")) {
    deltaOnlyMode = !deltaOnlyMode;
    addLog(deltaOnlyMode ? ">>> DELTA-FILTER AKTIV (Nur Änderungen)" : ">>> DELTA-FILTER DEAKTIVIERT (Zeige Alles)");
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }

  if (server.hasArg("clearCache")) {
    regCacheCount = 0;
    addLog(">>> SPEICHER RESET <<<");
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }

  if (!isSnifferMode) {
    if (server.hasArg("setMode")) {
      uint16_t newMode = server.arg("setMode").toInt();
      uint8_t res = writeRegFC10(0x03E8, newMode);
      if (res == node.ku8MBSuccess) {
        pwp.raw_mode = newMode;
        addLog("FC10 SUCCESS: Modus (0x03E8) -> " + String(newMode));
      } else {
        addLog("FC10 ERROR: Code 0x" + String(res, HEX));
      }
      server.sendHeader("Location", "/");
      server.send(303);
      return;
    }
    if (server.hasArg("setTemp")) {
      float newTemp = server.arg("setTemp").toFloat();
      uint16_t modbusVal = (uint16_t)(newTemp * 10.0);
      uint8_t res = writeRegFC10(0x03E9, modbusVal);
      if (res == node.ku8MBSuccess) {
        pwp.target_temp = newTemp;
        addLog("FC10 SUCCESS: Temp (0x03E9) -> " + String(newTemp, 1) + "°C");
      } else {
        addLog("FC10 ERROR: Code 0x" + String(res, HEX));
      }
      server.sendHeader("Location", "/");
      server.send(303);
      return;
    }
  }

  // HTML UI
  String html = "<html><head><meta charset='utf-8'><title>BWT Controller</title></head><body style='font-family:sans-serif; padding:15px; max-width:900px; margin:auto;'>";
  html += "<h2>BWT / Fairland Heatpump Controller</h2><hr>";
  
  html += "<p><b>Betriebsmodus:</b> ";
  if (isSnifferMode) {
    html += "<span style='color:blue; font-weight:bold;'>SNIFFER (Passiv)</span></p>";
    html += "<p><a href='/?mode=active'><button style='padding:8px; background:#d9534f; color:white;'>Wechsel zu ACTIVE MASTER</button></a> ";
    html += "<a href='/?toggleDelta=1'><button style='padding:8px;'>" + String(deltaOnlyMode ? "Zeige ALLE Pakete" : "NUR Änderungen (Delta)") + "</button></a> ";
    html += "<a href='/?clearCache=1'><button style='padding:8px; background:#f0ad4e; color:white;'>Reset Cache</button></a></p><hr>";
  } else {
    html += "<span style='color:red; font-weight:bold;'>ACTIVE MASTER (Steuerung)</span></p>";
    html += "<p><a href='/?mode=sniffer'><button style='padding:8px; background:#0275d8; color:white;'>Wechsel zu SNIFFER</button></a></p><hr>";
    
    html += "<h3>Steuerung (Write via FC10)</h3>";
    html += "<p><b>Aktueller Modus (Reg 0x03E8):</b> " + decodeRegValue(0x03E8, pwp.raw_mode) + "</p>";
    
    // Buttons für AUS, EIN, SMART, BOOST, ECO
    html += "<p><b>Schnellwahl Modus:</b><br>";
    html += "<a href='/?setMode=" + String(MODE_OFF) + "'><button style='padding:8px 12px; background:#d9534f; color:white; border:none; border-radius:4px; margin-right:4px; cursor:pointer;'>AUS (0)</button></a> ";
    html += "<a href='/?setMode=" + String(MODE_ON) + "'><button style='padding:8px 12px; background:#0275d8; color:white; border:none; border-radius:4px; margin-right:4px; cursor:pointer;'>EIN (1)</button></a> ";
    html += "<a href='/?setMode=" + String(MODE_SMART) + "'><button style='padding:8px 12px; background:#5bc0de; color:white; border:none; border-radius:4px; margin-right:4px; cursor:pointer;'>SMART (21)</button></a> ";
    html += "<a href='/?setMode=" + String(MODE_BOOST) + "'><button style='padding:8px 12px; background:#f0ad4e; color:white; border:none; border-radius:4px; margin-right:4px; cursor:pointer;'>BOOST (22)</button></a> ";
    html += "<a href='/?setMode=" + String(MODE_ECO) + "'><button style='padding:8px 12px; background:#5cb85c; color:white; border:none; border-radius:4px; cursor:pointer;'>ECO (23)</button></a></p>";

    html += "<form action='/' method='GET' style='margin-top:10px;'><b>Soll-Temperatur (0x03E9):</b> ";
    html += "<input type='number' step='0.5' name='setTemp' min='15' max='35' value='" + String(pwp.target_temp, 1) + "' style='width:60px; padding:4px;'> &deg;C ";
    html += "<input type='submit' value='Soll-Temp Senden'></form><hr>";
  }

  // Tabelle aller auf dem Bus gefundenen Register
  html += "<h3>Alle erkannten Modbus-Register (" + String(regCacheCount) + ")</h3>";
  html += "<table border='1' cellpadding='6' cellspacing='0' style='border-collapse:collapse; width:100%; text-align:left;'>";
  html += "<tr style='background:#f2f2f2;'><th>Register (Hex / Dez)</th><th>Bezeichnung</th><th>Aktueller Wert (Dekodiert)</th></tr>";
  
  for (int r = 0; r < regCacheCount; r++) {
    uint16_t a = regCache[r].addr;
    uint16_t v = regCache[r].val;
    html += "<tr><td>" + hex4(a) + " (" + String(a) + ")</td><td>" + getRegisterName(a) + "</td><td><b>" + decodeRegValue(a, v) + "</b></td></tr>";
  }
  html += "</table><hr>";
  
  html += "<h3>Live Modbus Console Logs</h3>";
  html += "<textarea id='logbox' style='width:100%; height:320px; font-family:monospace; background:#1e1e1e; color:#00ff00; padding:10px;' readonly></textarea>";
  
  html += "<script>";
  html += "setInterval(function() {";
  html += "  fetch('/logdata').then(r => r.text()).then(d => {";
  html += "    let b = document.getElementById('logbox');";
  html += "    if (b.value !== d) { b.value = d; b.scrollTop = b.scrollHeight; }";
  html += "  });";
  html += "}, 1500);";
  html += "</script></body></html>";
  
  server.send(200, "text/html", html);
}

void setup() {
  Serial.begin(115200);
  ModbusSerial.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);
  node.begin(17, ModbusSerial);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); }
  
  ArduinoOTA.setHostname("bwt-heatpump-esp32");
  ArduinoOTA.setPassword("11111111");
  ArduinoOTA.begin();

  server.on("/", handleRoot);
  server.on("/logdata", handleLogs); 
  server.begin();
  
  addLog("System gestartet. IP: " + WiFi.localIP().toString());
}

unsigned long lastModbusUpdate = 0;

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  
  if (isSnifferMode) {
    handleSniffer();
  } else {
    if (millis() - lastModbusUpdate > 5000) {
      updateActiveData();
      lastModbusUpdate = millis();
    }
  }
}
