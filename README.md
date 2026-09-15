# BWT / Fairland Pool-Wärmepumpen Modbus Controller & Sniffer (ESP32-C3)

Dieses Projekt ermöglicht das Auslesen und Steuern von **BWT** sowie **Fairland Inverter-Poolwärmepumpen** über die interne RS485 / Modbus RTU-Schnittstelle mittels eines **ESP32-C3**.

Das System bietet zwei Betriebsmodi:
1. **Sniffer-Modus (Passiv):** Liest den Datenverkehr zwischen dem originalen Tuya-WLAN-Modul und der Wärmepumpen-Elektronik mit, um Registeränderungen live im Web-Dashboard zu analysieren.
2. **Active Master Modus (Aktive Steuerung):** Übernimmt die Steuerung der Wärmepumpe direkt über Modbus RTU (Slave ID 17, Funktion `FC 0x10`).

---

## Inhaltsverzeichnis
- [Features](#-features)
- [Hardware & Verkabelung](#-hardware--verkabelung)
- [Modbus Register-Dokumentation](#-modbus-register-dokumentation)
- [Software & Abhängigkeiten](#-software--abhängigkeiten)
- [Inbetriebnahme & Flashen](#-inbetriebnahme--flashen)
- [Web-Dashboard & Bedienung](#-web-dashboard--bedienung)

---

## Features

* **Duales System:** Umschaltung zwischen passivem Sniffer und aktivem Master per Web-Button.
* **Erweiterter Delta-Filter:** Protokolliert im Sniffer-Modus gezielt Werteänderungen, um neue Modbus-Register schnell zu identifizieren.
* **Volle Sensorüberwachung:** Vorlauf, Rücklauf, Außentemperatur, Kompressorfrequenz, Kältemittel-, Verdampfer- und Heißgastemperatur.
* **Steuerung via Modbus Write (`FC 0x10`):** Zuverlässiges Ein-/Ausschalten, Moduswechsel (SMART / ECO) und Soll-Temperaturanpassung.
* **OTA-Updates (Over-The-Air):** Drahtlose Updates über WLAN, geschützt mit Passwort (`11111111`).
* **Responsive Web-Dashboard:** Live-Protokollierung via AJAX ohne Seiten-Reload.

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

> **Hinweis zum Active Master Modus:** Um Konflikte auf dem Modbus-Bus zu vermeiden, sollte das originale Tuya-WLAN-Modul abgezogen werden, wenn der ESP32 als *Active Master* agiert.

---

## Modbus Register-Dokumentation

### 1. Steuerung & Betriebsmodus (FC 0x03 Lesen / FC 0x10 Schreiben)

| Hex | Dez | Skalierung / Typ | Einheit | Beschreibung & Werte |
| :--- | :--- | :--- | :--- | :--- |
| **`0x03E8`** | **1000** | uint16 | Enum | **Betriebsmodus / Power**<br>• `0` = AUS (Standby)<br>• `1` = EIN <br>• `21` (`0x15`) = **ECO**<br>• `23` (`0x17`) = **SMART**<br>• `22` (`0x16`) = **BOOST** |
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

## 🌐 Web-Dashboard & Bedienung

Rufe im Browser die IP-Adresse des ESP32 auf (z. B. `http://192.168.0.37`):

1. **Sniffer Modus:**
   * Klicke auf **„NUR Änderungen (Delta)“**, um die Konsole übersichtlich zu halten. Sobald am Display oder in der Tuya-App Werte geändert werden, wird die genaue Registeradresse mit Hex- und Dezimalwert geloggt.
2. **Active Master Modus:**
   * Nach dem Abziehen des Tuya-Moduls schaltest du im Web-Dashboard auf **ACTIVE MASTER** um.
   * Sämtliche Sensoren werden in einer übersichtlichen Tabelle dargestellt.
   * Über die Steuerungsschaltflächen können die Betriebsmodi (*AUS*, *SMART*, *ECO/BOOST*) sowie die Soll-Temperatur direkt über Modbus `FC 0x10` gesetzt werden.
