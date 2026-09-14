# 🏊‍♂️ BWT / Fairland Pool-Wärmepumpen Modbus Controller & Sniffer (ESP32-C3)

Dieses Projekt ermöglicht das Auslesen und Steuern von **BWT** sowie **Fairland Inverter-Poolwärmepumpen** über die interne RS485 / Modbus RTU-Schnittstelle mittels eines **ESP32-C3**.

Das System bietet zwei Betriebsmodi:
1. **Sniffer-Modus (Passiv):** Liest den Datenverkehr zwischen dem originalen Tuya-WLAN-Modul und der Wärmepumpen-Elektronik mit, um Registeränderungen live im Web-Dashboard zu analysieren.
2. **Active Master Modus (Aktive Steuerung):** Übernimmt die Steuerung der Wärmepumpe direkt über Modbus RTU (Slave ID 17, Funktion `FC 0x10`).

---

## 📑 Inhaltsverzeichnis
- [Features](#-features)
- [Hardware & Verkabelung](#-hardware--verkabelung)
- [Modbus Register-Dokumentation](#-modbus-register-dokumentation)
- [Software & Abhängigkeiten](#-software--abhängigkeiten)
- [Inbetriebnahme & Flashen](#-inbetriebnahme--flashen)
- [Vollständiger Quellcode](#-vollständiger-quellcode)
- [Web-Dashboard & Bedienung](#-web-dashboard--bedienung)

---

## ✨ Features

* **Duales System:** Umschaltung zwischen passivem Sniffer und aktivem Master per Web-Button.
* **Erweiterter Delta-Filter:** Protokolliert im Sniffer-Modus gezielt Werteänderungen, um neue Modbus-Register schnell zu identifizieren.
* **Volle Sensorüberwachung:** Vorlauf, Rücklauf, Außentemperatur, Kompressorfrequenz, Kältemittel-, Verdampfer- und Heißgastemperatur.
* **Steuerung via Modbus Write (`FC 0x10`):** Zuverlässiges Ein-/Ausschalten, Moduswechsel (SMART / ECO) und Soll-Temperaturanpassung.
* **OTA-Updates (Over-The-Air):** Drahtlose Updates über WLAN, geschützt mit Passwort (`11111111`).
* **Responsive Web-Dashboard:** Live-Protokollierung via AJAX ohne Seiten-Reload.

---

## 🔌 Hardware & Verkabelung

### Benötigte Komponenten
* **Microcontroller:** ESP32-C3 (z. B. SuperMini oder DevModule)
* **RS485-Transceiver:** TTL-zu-RS485 Modul (z. B. MAX485 oder Auto-Flow RS485 Modul)
* **Spannungsversorgung:** 5V DC (über USB oder ein Interne-Netzteilplatine der Wärmepumpe)

### Pin-Belegung (ESP32-C3)

| ESP32-C3 Pin | RS485 Modul Pin | Beschreibung |
| :--- | :--- | :--- |
| **GPIO 2** | `RO` (RX) | Empfangsleitung Modbus |
| **GPIO 3** | `DI` (TX) | Sendeleitung Modbus |
| **5V / 3.3V** | `VCC` | Stromversorgung Transceiver |
| **GND** | `GND` | Gemeinsame Masse |

### RS485 Verbindung zur Wärmepumpe (Slave ID: 17, 9600 Baud, 8N1)
* **RS485 `A` (D+)** → Klemme `A` an der Wärmepumpen-Platine / Tuya-Stecker
* **RS485 `B` (D-)** → Klemme `B` an der Wärmepumpen-Platine / Tuya-Stecker

> **Hinweis zum Active Master Modus:** Um Konflikte auf dem Modbus-Bus zu vermeiden, sollte das originale Tuya-WLAN-Modul abgezogen werden, wenn der ESP32 als *Active Master* agiert.

---

## 📊 Modbus Register-Dokumentation

### 1. Steuerung & Betriebsmodus (FC 0x03 Lesen / FC 0x10 Schreiben)

| Hex | Dez | Skalierung / Typ | Einheit | Beschreibung & Werte |
| :--- | :--- | :--- | :--- | :--- |
| **`0x03E8`** | **1000** | uint16 | Enum | **Betriebsmodus / Power**<br>• `0` = AUS (Standby)<br>• `18` (`0x12`) = **SMART**<br>• `23` (`0x17`) = **ECO / BOOST** |
| **`0x03E9`** | **1001** | Wert / 10.0 | °C | **Soll-Temperatur (Set Point)**<br>• z. B. `320` = **32,0 °C** (Einstellbereich: 15.0 – 35.0 °C) |

---

### 2. Temperatursensoren (FC 0x03 Lesen)

| Hex | Dez | Skalierung / Typ | Einheit | Sensor / Messstelle |
| :--- | :--- | :--- | :--- | :--- |
| **`0x0200`** | **512** | Wert / 10.0 | °C | **Wassereintritts-Temperatur** (Vorlauf) |
| **`0x0201`** | **513** | Wert / 10.0 | °C | **Wasseraustritts-Temperatur** (Rücklauf) |
| **`0x0203`** | **515** | Wert / 10.0 | °C | **Echte Außentemperatur** (Umgebungssensor) |
| **`0x0209`** | **521** | Wert / 10.0 | °C | **Saugrohr- / Kältemitteltemperatur** (Ansaugseite) |
| **`0x020A`** | **522** | Wert / 10.0 | °C | **Verdampfer- / Abtausensor** (Coil Temperature) |
| **`0x0044`** | **68** | Wert / 10.0 | °C | **Heißgas-Temperatur** (Verdichter-Austritt) |
| **`0x01F8`** | **504** | Wert / 10.0 | °C | **Inverter-Kühlkörper- / Hilfstemperatur** |

---

### 3. Betriebswerte & Status-Register (FC 0x03 Lesen)

| Hex | Dez | Skalierung / Typ | Einheit | Beschreibung |
| :--- | :--- | :--- | :--- | :--- |
| **`0x01FE`** | **510** | Wert / 10.0 | Hz | **Kompressor-Frequenz** (z. B. `560` = **56,0 Hz**) |
| **`0x01F5`** | **501** | uint16 | Flag | **Kompressor-Status** (`0` = Inaktiv / AUS) |
| **`0x01F7`** | **503** | uint16 | Flag | **Lüfter- / Systemaktivität** (`0` = Inaktiv / AUS) |
| **`0x01FB`** | **507** | Hex-Value | Code | **Standby- / Ausschalt-Marker** (`0xFB00` / `64256` = AUS) |
| **`0x01F4`** | **500** | uint16 | ID | **Geräte-ID / Firmware-Codierung** (z. B. `1043`) |

---

## 💻 Software & Abhängigkeiten

### Benötigte Arduino-Bibliotheken
Installiere folgende Bibliotheken über den **Arduino Library Manager**:
1. **`ModbusMaster`** von 4-20ma
2. **`WiFi`** (Standardmäßig im ESP32-Board-Packet enthalten)
3. **`WebServer`** (Standardmäßig im ESP32-Board-Packet enthalten)
4. **`ArduinoOTA`** (Standardmäßig im ESP32-Board-Packet enthalten)

---

## 🚀 Inbetriebnahme & Flashen

### 1. Erstes Flashen via USB
1. Verbinde den ESP32-C3 per USB mit deinem Mac/PC.
2. Trage deine WLAN-Zugangsdaten in den Code ein (`ssid` und `password`).
3. Sollte die Arduino IDE den Fehler `could not open port /dev/cu.usbmodem...` anzeigen:
   * Halte die **BOOT-Taste** auf dem ESP32 gedrückt.
   * Klicke in der Arduino IDE auf **Hochladen**.
   * Lasse die BOOT-Taste erst los, wenn in der Konsole `Connecting........` erscheint.

### 2. Drahtlose OTA-Updates
Nach dem ersten USB-Flash kannst du zukünftige Updates drahtlos über das Netzwerk einspielen:
* Wähle unter **Werkzeuge → Port** den Netzwerkport `bwt-heatpump-esp32 at <IP-Adresse>`.
* Klicke auf **Hochladen**.
* Wenn die IDE nach einem Passwort fragt, gib **`11111111`** ein.

---

## 📄 Vollständiger Quellcode (`.ino`)

```cpp
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

WebServer server(80);
ModbusMaster node;
HardwareSerial ModbusSerial(1);

// --- Betriebsmodus ---
bool isSnifferMode = true;
bool deltaOnlyMode = true;

// --- Web-Logger (Ringpuffer) ---
const int MAX_LOG_LINES = 25;
String logBuffer[MAX_LOG_LINES];
int logIndex = 0;

void addLog(String msg) {
  Serial.println(msg); 
  logBuffer[logIndex] = msg;
  logIndex = (logIndex + 1) % MAX_LOG_LINES;
}

// --- Register Cache für Delta-Filter ---
struct RegisterState {
  uint16_t addr;
  uint16_t val;
};
RegisterState regCache[64];
int regCacheCount = 0;
uint16_t lastRequestedAddr = 0xFFFF;

// --- Active Mode Datenstruktur (Slave ID 17) ---
struct PWP_State {
  uint16_t raw_mode = 0;          // Register 0x03E8 (1000): 0=AUS, 18=SMART, 23=ECO/BOOST
  float target_temp = 28.0;       // Register 0x03E9 (1001) -> Wert / 10.0
  float water_in_temp = 0.0;      // Register 0x0200 (512) -> Vorlauf (°C)
  float water_out_temp = 0.0;     // Register 0x0201 (513) -> Rücklauf (°C)
  float outdoor_temp = 0.0;       // Register 0x0203 (515) -> Echte Außentemperatur (°C)
  float compressor_freq = 0.0;    // Register 0x01FE (510) -> Kompressor (Hz)
  float suction_temp = 0.0;       // Register 0x0209 (521) -> Kältemittel/Saugrohr (°C)
  float hotgas_temp = 0.0;        // Register 0x0044 (68)  -> Heißgas/Verdichter (°C)
  float evap_temp = 0.0;          // Register 0x020A (522) -> Verdampfer (°C)
  uint16_t status_code = 0;       // Register 0x01FB (507) -> Status/Standby-Marker
} pwp;

// --- Sniffer Puffer ---
#define FRAME_TIMEOUT 40
uint8_t snifferFrame[256];
uint16_t frameLen = 0;
unsigned long lastByteTime = 0;

void flushModbus() {
  while(ModbusSerial.available()) ModbusSerial.read();
}

// Hilfsfunktion: Schreiben mit FC 0x10 (Write Multiple Registers)
uint8_t writeRegFC10(uint16_t regAddress, uint16_t value) {
  node.setTransmitBuffer(0, value);
  return node.writeMultipleRegisters(regAddress, 1);
}

// --- Sniffer-Engine (Liest FC 0x03 und FC 0x10) ---
void processSnifferFrame(uint8_t* buf, uint16_t len) {
  // 1. FC 0x10 (Schreibbefehl vom Tuya-Modul an ID 17)
  if (buf[0] == 0x11 && buf[1] == 0x10 && len >= 9) {
    uint16_t writeAddr = (buf[2] << 8) | buf[3];
    uint16_t writeVal  = (buf[7] << 8) | buf[8];
    addLog("✍️ MASTER WRITE (FC10) | Reg 0x" + String(writeAddr, HEX) + " (" + String(writeAddr) + ") = " + String(writeVal) + " [Hex: 0x" + String(writeVal, HEX) + "]");
    return;
  }

  // 2. FC 0x03 Lese-Anfrage vom Tuya Modul
  if (buf[0] == 0x11 && buf[1] == 0x03 && len == 8) {
    lastRequestedAddr = (buf[2] << 8) | buf[3];
    return;
  }
  
  // 3. FC 0x03 Lese-Antwort der Wärmepumpe
  if (buf[0] == 0x11 && buf[1] == 0x03 && len >= 5 && lastRequestedAddr != 0xFFFF) {
    uint8_t byteCount = buf[2];
    for (int i = 0; i < byteCount; i += 2) {
      if ((3 + i + 1) >= len) break;
      uint16_t currentAddr = lastRequestedAddr + (i / 2);
      uint16_t currentVal = (buf[3 + i] << 8) | buf[4 + i];
      
      int foundIdx = -1;
      for (int r = 0; r < regCacheCount; r++) {
        if (regCache[r].addr == currentAddr) {
          foundIdx = r;
          break;
        }
      }
      
      if (foundIdx == -1 && regCacheCount < 64) {
        regCache[regCacheCount] = {currentAddr, currentVal};
        regCacheCount++;
        addLog("NEUES REG | 0x" + String(currentAddr, HEX) + " (" + String(currentAddr) + ") = " + String(currentVal) + " [Hex: 0x" + String(currentVal, HEX) + "]");
      } 
      else if (foundIdx != -1) {
        if (regCache[foundIdx].val != currentVal) {
          addLog("🔥 ÄNDERUNG! Reg 0x" + String(currentAddr, HEX) + " (" + String(currentAddr) + "): ALT=" + String(regCache[foundIdx].val) + " -> NEU=" + String(currentVal) + " [0x" + String(currentVal, HEX) + "]");
          regCache[foundIdx].val = currentVal;
        } else if (!deltaOnlyMode) {
          addLog("RAW Reg 0x" + String(currentAddr, HEX) + " = " + String(currentVal));
        }
      }
    }
    lastRequestedAddr = 0xFFFF;
    return;
  }

  if (!deltaOnlyMode) {
    String raw = "RAW: ";
    for(int i=0; i<len; i++) {
      if (buf[i] < 0x10) raw += "0";
      raw += String(buf[i], HEX) + " ";
    }
    addLog(raw);
  }
}

void handleSniffer() {
  while (ModbusSerial.available()) {
    snifferFrame[frameLen++] = ModbusSerial.read();
    lastByteTime = millis();
    if (frameLen >= 256) frameLen = 255;
  }
  if (frameLen > 0 && (millis() - lastByteTime > FRAME_TIMEOUT)) {
    processSnifferFrame(snifferFrame, frameLen);
    frameLen = 0;
  }
}

// --- Active Master Logik (Abfrage aller Sensoren an ID 17) ---
void updateActiveData() {
  // 1. Vorlauf (0x0200) & Rücklauf (0x0201)
  if (node.readHoldingRegisters(0x0200, 2) == node.ku8MBSuccess) {
    pwp.water_in_temp = node.getResponseBuffer(0) / 10.0;
    pwp.water_out_temp = node.getResponseBuffer(1) / 10.0;
  }
  delay(20);

  // 2. Echte Außentemperatur (0x0203 = 515)
  if (node.readHoldingRegisters(0x0203, 1) == node.ku8MBSuccess) {
    int16_t rawOut = (int16_t)node.getResponseBuffer(0);
    pwp.outdoor_temp = rawOut / 10.0;
  }
  delay(20);

  // 3. Kompressor-Frequenz (0x01FE = 510)
  if (node.readHoldingRegisters(0x01FE, 1) == node.ku8MBSuccess) {
    pwp.compressor_freq = node.getResponseBuffer(0) / 10.0;
  }
  delay(20);

  // 4. Kältemittel / Saugrohr (0x0209) & Verdampfer (0x020A)
  if (node.readHoldingRegisters(0x0209, 2) == node.ku8MBSuccess) {
    pwp.suction_temp = node.getResponseBuffer(0) / 10.0;
    pwp.evap_temp = node.getResponseBuffer(1) / 10.0;
  }
  delay(20);

  // 5. Heißgas / Verdichter-Austritt (0x0044 = 68)
  if (node.readHoldingRegisters(0x0044, 1) == node.ku8MBSuccess) {
    pwp.hotgas_temp = node.getResponseBuffer(0) / 10.0;
  }
  delay(20);

  // 6. Status-Code / Standby-Marker (0x01FB = 507)
  if (node.readHoldingRegisters(0x01FB, 1) == node.ku8MBSuccess) {
    pwp.status_code = node.getResponseBuffer(0);
  }
  delay(20);

  // 7. Power/Modus (0x03E8) & Soll-Temp (0x03E9)
  if (node.readHoldingRegisters(0x03E8, 2) == node.ku8MBSuccess) {
    pwp.raw_mode = node.getResponseBuffer(0);
    pwp.target_temp = node.getResponseBuffer(1) / 10.0;

    String modeName = "UNBEKANNT (" + String(pwp.raw_mode) + ")";
    if (pwp.raw_mode == 0) modeName = "AUS";
    else if (pwp.raw_mode == 18) modeName = "SMART (18)";
    else if (pwp.raw_mode == 23) modeName = "ECO / BOOST (23)";

    addLog("Active Poll: Modus=" + modeName + " | Soll=" + String(pwp.target_temp, 1) + "°C | Vorlauf=" + String(pwp.water_in_temp, 1) + "°C | Rücklauf=" + String(pwp.water_out_temp, 1) + "°C | Außen=" + String(pwp.outdoor_temp, 1) + "°C");
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
  // Modus umschalten
  if (server.hasArg("mode")) {
    String m = server.arg("mode");
    if (m == "sniffer") {
      isSnifferMode = true;
      addLog("=== MODUS WECHSEL: SNIFFER (Passiv) ===");
    } else if (m == "active") {
      isSnifferMode = false;
      addLog("=== MODUS WECHSEL: ACTIVE MASTER (ID 17) ===");
    }
    flushModbus();
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }

  // Sniffer Steuerung
  if (server.hasArg("toggleDelta")) {
    deltaOnlyMode = !deltaOnlyMode;
    addLog(deltaOnlyMode ? ">>> DELTA-FILTER AKTIV (Nur Änderungen)" : ">>> DELTA-FILTER DEAKTIVIERT (Zeige Alles)");
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }
  if (server.hasArg("clearCache")) {
    regCacheCount = 0;
    addLog(">>> SPEICHER RESET (Neu anlernen) <<<");
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }

  // Active Master Befehle mit FC 0x10 Senden
  if (!isSnifferMode) {
    if (server.hasArg("setMode")) {
      uint16_t newMode = server.arg("setMode").toInt();
      uint8_t res = writeRegFC10(0x03E8, newMode);
      if (res == node.ku8MBSuccess) {
        pwp.raw_mode = newMode;
        addLog("FC10 ERFOLG: Modus (Reg 0x03E8) auf " + String(newMode) + " gesetzt!");
      } else {
        addLog("FC10 FEHLER beim Modus-Schreiben: Code 0x" + String(res, HEX));
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
        addLog("FC10 ERFOLG: Soll-Temp (Reg 0x03E9) auf " + String(newTemp, 1) + "°C gesetzt!");
      } else {
        addLog("FC10 FEHLER beim Temp-Schreiben: Code 0x" + String(res, HEX));
      }
      server.sendHeader("Location", "/");
      server.send(303);
      return;
    }
  }

  // HTML Dashboard
  String html = "<html><head><meta charset='utf-8'><title>BWT Heatpump Controller</title></head><body style='font-family:sans-serif; padding:20px; max-width: 800px; margin: auto;'>";
  html += "<h2>BWT / Fairland Controller (Slave ID 17)</h2><hr>";
  
  html += "<p><b>Betriebsmodus:</b> ";
  if (isSnifferMode) {
    html += "<span style='color:blue; font-weight:bold;'>SNIFFER (Passiv - Tuya-Modul aktiv)</span></p>";
    html += "<p><a href='/?mode=active'><button style='padding:10px; background:#d9534f; color:white; border:none; border-radius:4px; cursor:pointer;'>Wechsel zu ACTIVE MASTER (Tuya abziehen!)</button></a> ";
    html += "<a href='/?toggleDelta=1'><button style='padding:10px;'>" + String(deltaOnlyMode ? "Zeige ALLE RAW Pakete" : "NUR Änderungen (Delta)") + "</button></a> ";
    html += "<a href='/?clearCache=1'><button style='padding:10px; background:#f0ad4e; color:white; border:none; border-radius:4px;'>Reset Cache</button></a></p>";
  } else {
    html += "<span style='color:red; font-weight:bold;'>ACTIVE MASTER (ESP32 steuert Pumpe)</span></p>";
    html += "<p><a href='/?mode=sniffer'><button style='padding:10px; background:#0275d8; color:white; border:none; border-radius:4px; cursor:pointer;'>Wechsel zu SNIFFER (Passiv)</button></a></p><hr>";
    
    // Steuer-Bedienfeld
    html += "<h3>Steuerung (Write via FC 0x10)</h3>";
    
    String modeTxt = "AUS";
    if (pwp.raw_mode == 18) modeTxt = "SMART (18 / 0x12)";
    if (pwp.raw_mode == 23) modeTxt = "ECO / BOOST (23 / 0x17)";
    
    html += "<p><b>Betriebsstatus (Reg 0x03E8):</b> <span style='color:green; font-weight:bold;'>" + modeTxt + "</span></p>";
    html += "<p>Modus schalten: ";
    html += "<a href='/?setMode=0'><button style='padding:8px 15px; background:#d9534f; color:white; margin-right:5px;'>AUS (0)</button></a> ";
    html += "<a href='/?setMode=18'><button style='padding:8px 15px; background:#5bc0de; color:white; margin-right:5px;'>SMART (18)</button></a> ";
    html += "<a href='/?setMode=23'><button style='padding:8px 15px; background:#5cb85c; color:white;'>ECO / BOOST (23)</button></a></p>";
    
    html += "<br><form action='/' method='GET'><b>Soll-Temperatur (Reg 0x03E9):</b> ";
    html += "<input type='number' step='0.5' name='setTemp' min='15' max='35' value='" + String(pwp.target_temp, 1) + "' style='padding:5px; width:70px;'> &deg;C ";
    html += "<input type='submit' value='Soll-Temp Senden (FC10)' style='padding:6px 12px;'></form><hr>";

    // Sensor-Anzeige Dashboard
    html += "<h3>Dekodierte Sensorwerte</h3>";
    html += "<table border='1' cellpadding='8' cellspacing='0' style='border-collapse:collapse; width:100%; text-align:left;'>";
    html += "<tr style='background:#f2f2f2;'><th>Sensor / Messwert</th><th>Register (Hex / Dez)</th><th>Aktueller Wert</th></tr>";
    html += "<tr><td>Wassereintritt (Vorlauf)</td><td>0x0200 (512)</td><td><b>" + String(pwp.water_in_temp, 1) + " &deg;C</b></td></tr>";
    html += "<tr><td>Wasseraustritt (Rücklauf)</td><td>0x0201 (513)</td><td><b>" + String(pwp.water_out_temp, 1) + " &deg;C</b></td></tr>";
    html += "<tr><td>Außentemperatur</td><td>0x0203 (515)</td><td><b>" + String(pwp.outdoor_temp, 1) + " &deg;C</b></td></tr>";
    html += "<tr><td>Kompressor-Frequenz</td><td>0x01FE (510)</td><td><b>" + String(pwp.compressor_freq, 1) + " Hz</b></td></tr>";
    html += "<tr><td>Kältemittel / Saugrohr</td><td>0x0209 (521)</td><td><b>" + String(pwp.suction_temp, 1) + " &deg;C</b></td></tr>";
    html += "<tr><td>Verdampfer / Abtausensor</td><td>0x020A (522)</td><td><b>" + String(pwp.evap_temp, 1) + " &deg;C</b></td></tr>";
    html += "<tr><td>Heißgas / Verdichter-Austritt</td><td>0x0044 (68)</td><td><b>" + String(pwp.hotgas_temp, 1) + " &deg;C</b></td></tr>";
    html += "<tr><td>Status-Code / Standby</td><td>0x01FB (507)</td><td><b>0x" + String(pwp.status_code, HEX) + " (" + String(pwp.status_code) + ")</b></td></tr>";
    html += "</table><hr>";
  }
  
  html += "<h3>Live Modbus Console Logs</h3>";
  html += "<textarea id='logbox' style='width:100%; height:300px; font-family:monospace; background:#1e1e1e; color:#00ff00; padding:10px;' readonly></textarea>";
  
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
  node.begin(17, ModbusSerial); // Modbus Slave ID 17

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); }
  
  // Arduino OTA Initialisierung mit Passwort "11111111"
  ArduinoOTA.setHostname("bwt-heatpump-esp32");
  ArduinoOTA.setPassword("11111111");
  ArduinoOTA.onStart([]() { addLog("OTA Update gestartet..."); });
  ArduinoOTA.onEnd([]() { addLog("OTA Update erfolgreich beendet!"); });
  ArduinoOTA.begin();

  server.on("/", handleRoot);
  server.on("/logdata", handleLogs); 
  server.begin();
  
  addLog("System bereit. IP: " + WiFi.localIP().toString());
  addLog("OTA Passwort geschützt ('11111111'). Hostname: bwt-heatpump-esp32");
}

unsigned long lastModbusUpdate = 0;

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  
  if (isSnifferMode) {
    handleSniffer();
  } else {
    // Im Active Master Modus alle 4 Sekunden Daten abfragen
    if (millis() - lastModbusUpdate > 4000) {
      updateActiveData();
      lastModbusUpdate = millis();
    }
  }
}
```

---

## 🌐 Web-Dashboard & Bedienung

Rufe im Browser die IP-Adresse des ESP32 auf (z. B. `http://192.168.0.37`):

1. **Sniffer Modus:**
   * Klicke auf **„NUR Änderungen (Delta)“**, um die Konsole übersichtlich zu halten. Sobald am Display oder in der Tuya-App Werte geändert werden, wird die genaue Registeradresse mit Hex- und Dezimalwert geloggt.
2. **Active Master Modus:**
   * Nach dem Abziehen des Tuya-Moduls schaltest du im Web-Dashboard auf **ACTIVE MASTER** um.
   * Sämtliche Sensoren werden in einer übersichtlichen Tabelle dargestellt.
   * Über die Steuerungsschaltflächen können die Betriebsmodi (*AUS*, *SMART*, *ECO/BOOST*) sowie die Soll-Temperatur direkt über Modbus `FC 0x10` gesetzt werden.
