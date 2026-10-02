# BWT / Fairland Pool-Wärmepumpen Modbus Controller & Sniffer (ESP32-C3)

Dieses Projekt ermöglicht das Auslesen und Steuern von **BWT** sowie **Fairland Inverter-Poolwärmepumpen** über die interne RS485 / Modbus RTU-Schnittstelle mittels eines **ESP32-C3**.

Dieses Projekt wurde nötig weil das orginale App und das WLAN Modul des Herstellers ist nicht zuverlässig arbeiten, es reagiert teils gar nicht - liefert nicht nachvollziehbare WLAN Fehler - Cloud Server ist nicht erreichbar ... Das hier ist also mein Plan B.

Das System arbeitet als **Passiv-Sniffer / Modbus-Master**: Es liest den laufenden Datenverkehr zwischen dem originalen Tuya-Bedienteil (Master) und der Wärmepumpe (Slave `0x11`) mit und ermöglicht das zwischenschalten eigener Steuerbefehle direkt über Modbus RTU (Slave ID 17, Funktion `FC 0x10`).

---

## Inhaltsverzeichnis
- [Features](#features)
- [Hardware & Verkabelung](#hardware--verkabelung)
- [Modbus Register-Dokumentation](#modbus-register-dokumentation)
- [Software & Abhängigkeiten](#software--abhängigkeiten)
- [Inbetriebnahme & Flashen](#inbetriebnahme--flashen)
- [FHEM Integration](#fhem-integration)
- [Anpassungen](#anpassungen)
- [Beispiel Log](#log)
---

## Features

* **Passive Modbus-Sniffer Engine:**
  * Liest alle Modbus-Frames (Read FC03 / Write FC10) in Echtzeit mit.
  * Dynamisches Inter-Byte-Timeout (3 ms) und 1024-Byte Ringpuffer für saubere Paket-Trennung ohne Datenverlust.
  * Erkennt Modbus-Register-Änderungen sofort und spiegelt sie auf MQTT.
* **Hybrider Master-Modus (Steuerung):**
  * Ein-/Ausschalten (`power`)
  * Betriebsmodus ändern (`mode`: `ECO`, `SMART`, `BOOST`)
  * Soll-Temperatur einstellen (`target_temp`: 28.0 – 35.0 °C)
  * Manuelle Abfrage auslösen (`poll`)
* **Echtzeit Web-Interface (3 Views):**
  * **Desktop Dashboard & Mobile Remote:** Übersichtliche Steuerung für Smartphone und PC.
  * **Advanced Diagnostics:** Detaillierte Systemwerte, Heap-Speicher, Signalstärke & Register.
  * **Live Log Console:**
    * Zeitstempel im Format `[HH:MM]` via NTP (automatische Sommer-/Winterzeit).
    * Filterbar nach `ALL`, `HUMAN` (Klartext) und `RAW` (Hex-Dump).
    * Konfigurierbarer Zeilenpuffer (Standard: 500 Zeilen, persistente Einstellungen via Browser-`localStorage`).
* **MQTT & Smart Home Integration:**
  * Vollständig kompatibel mit **FHEM** (`MQTT2_DEVICE`), **Home Assistant**, **ioBroker** und **OpenHAB**.
  * Automatische Status-Updates bei Register-Änderungen.    
* **OTA-Updates (Over-The-Air):** Drahtlose Updates über WLAN, geschützt mit Passwort (`11111111`).

---

## Hardware & Verkabelung

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

> **Hinweis zum Active Master:** Das originale Tuya-WLAN-Modul agiert weiterhin als Active Master. Unser ESP32 ist via Zwischenstecker als "Man in the Middle" geschaltet und liest alle Befehle mit und verhält sich selbst ruhig - kann aber auf Anforderung hin auch selbet Befehle senden (Register Poll / Modus setzen ...). 

---

## Modbus Register-Dokumentation

### 1. Steuerung & Betriebsmodus (FC 0x03 Lesen / FC 0x10 Schreiben)

| Hex | Dez | Skalierung / Typ | Einheit | Beschreibung & Werte |
| :--- | :--- | :--- | :--- | :--- |
| **`0x03E8`** | **1000** | uint16 | Enum | **Betriebsmodus / Power**<br>• `0` = AUS (Standby)<br>• `18` = EIN <br>• `21` (`0x15`) = **ECO**<br>• `23` (`0x17`) = **SMART**<br>• `22` (`0x16`) = **BOOST** |
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

## Software & Abhängigkeiten

### Benötigte Arduino-Bibliotheken
Installiere folgende Bibliotheken über den **Arduino Library Manager**:
1. **`ModbusMaster`** von 4-20ma
2. **`WiFi`** (Standardmäßig im ESP32-Board-Packet enthalten)
3. **`WebServer`** (Standardmäßig im ESP32-Board-Packet enthalten)
4. **`ArduinoOTA`** (Standardmäßig im ESP32-Board-Packet enthalten)

---

## Inbetriebnahme & Flashen

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

## FHEM Integration

Zur Anbindung in FHEM erstelle ein neues `MQTT2_DEVICE` und verknüpfe es mit deinem bestehenden `MQTT2_CLIENT` (Ersetze `DEIN_MQTT_CLIENT_NAME` durch den Namen deines MQTT-IO-Devices):

```
defmod WP_Waermepumpe MQTT2_DEVICE
attr WP_Waermepumpe IODev DEIN_MQTT_CLIENT_NAME

attr WP_Waermepumpe setList \
  power:ON,OFF heatpump/set/power $EVTPART1\
  mode:ECO,SMART,BOOST heatpump/set/mode $EVTPART1\
  target_temp:slider,15.0,0.5,35.0,1 heatpump/set/target_temp $EVTPART1\
  poll:noArg heatpump/set/poll 1

attr WP_Waermepumpe readingList \
  heatpump/status:.* status\
  heatpump/power:.* power\
  heatpump/mode:.* mode\
  heatpump/target_temp:.* target_temp\
  heatpump/water_in_temp:.* water_in_temp\
  heatpump/water_out_temp:.* water_out_temp\
  heatpump/outdoor_temp:.* outdoor_temp\
  heatpump/compressor_freq:.* compressor_freq

attr WP_Waermepumpe webCmd power:mode:target_temp:poll
attr WP_Waermepumpe webCmdLabel Power:Modus:Soll-Temp:Abfrage
attr WP_Waermepumpe widgetOverride target_temp:slider,15.0,0.5,35.0,1
attr WP_Waermepumpe stateFormat Mode: mode | Soll: target_temp °C | Status: power
```


An- / Ausschalten:
```
set DEIN_MQTT_CLIENT_NAME publish heatpump/set/power ON
```

Soll-Temperatur ändern (z. B. 28.5 °C):
```
set DEIN_MQTT_CLIENT_NAME publish heatpump/set/target_temp 28.5
```

Modus ändern (ECO, SMART, BOOST):
```
set DEIN_MQTT_CLIENT_NAME publish heatpump/set/mode SMART
```

Manuelle Register-Abfrage auslösen:
```
set DEIN_MQTT_CLIENT_NAME publish heatpump/set/poll 1
```

---


## Anpassungen

```
// Log-Speicher & Puffer
const int MAX_LOG_LINES = 1000;     // Anzahl Zeilen im Web-Log (ca. 80-150 KB RAM)
#define FRAME_TIMEOUT_MS 3          // Inter-Byte Timeout für 9600 Baud Modbus

// WLAN & MQTT
const char* ssid = "DEIN_WLAN_SSID";
const char* password = "DEIN_WLAN_PASSWORT";
const char* mqtt_server = "192.168.1.X";

// Setup
ArduinoOTA.setPassword("11111111");
```

---

## Log

```
[SNIFF] 📥 READ Reg 0x0200 (Vorlauf) = 17,0 °C
[SNIFF] 📥 READ Reg 0x0203 (Außen) = 16,5 °C
[SNIFF] ✍️ WRITE FC10 | Reg 0x03E9 (Soll-Temp) = 28,0 °C (0x0118)
📥 MQTT Empfangen [heatpump/set/target_temp]: 28.5
```
