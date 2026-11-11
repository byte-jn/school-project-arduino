# Arduino-Projekte

## Projekte

| Projekt | Board | Beschreibung |
|---|---|---|
| [Reaktionszeit](Reaktionszeit/Reaktionszeit.txt) | Arduino Uno (AVR) | LED leuchtet nach 1–3 s zufällig auf, Taster stoppt die Zeit, Ausgabe in ms im Serial Monitor. |
| [Temperatursensor](Temperatursensor/Temperatursensor.txt) | Arduino Uno (AVR) | DHT11 misst die Temperatur, Ampel-LEDs zeigen grün (bis 25 °C), gelb (über 25 °C) oder rot (über 26 °C). |
| [AnalogeDatenauslesung](AnalogeDatenauslesung/AnalogeDatenauslesung.txt) | Arduino Uno (AVR) | Potentiometer an A3 wird ausgelesen, drei LEDs (rot, gelb, blau) zeigen je ein Drittel des Wertebereichs, Rohwert im Serial Monitor. |
| [Wiederstandsmessen](Wiederstandsmessen/Wiederstandsmessen.txt) | Arduino Uno (AVR) | Platzhalter: schaltet bisher nur die eingebaute LED ein. |
| [LaptopKuehlung_Sensor](LaptopKuehlung_Sensor/LaptopKuehlung_Sensor.ino) | NodeMCU 1.0 (ESP8266) | Laptop-Kühlung, Knoten 1: DHT22 misst alle 5 s, sendet die Temperatur per MQTT, LED-Ampel zeigt den Zustand. |
| [LaptopKuehlung_Luefter](LaptopKuehlung_Luefter/LaptopKuehlung_Luefter.ino) | NodeMCU 1.0 (ESP8266) | Laptop-Kühlung, Knoten 2: schaltet den Lüfter anhand der Temperatur (mit Hysterese und Fail-safe). |

## Dependencies

### Board-Cores (Boardverwalter)

| Core | Version | Für | Installation |
|---|---|---|---|
| Arduino AVR Boards | 1.8.8 | Reaktionszeit, Temperatursensor, AnalogeDatenauslesung, Wiederstandsmessen | Bei der Arduino IDE dabei |
| esp8266 by ESP8266 Community | 3.1.2 | LaptopKuehlung_Sensor, LaptopKuehlung_Luefter | Siehe unten |

ESP8266-Core installieren:

1. **Datei → Einstellungen → Zusätzliche Boardverwalter-URLs**:
   `https://arduino.esp8266.com/stable/package_esp8266com_index.json`
   (Quelle: [esp8266/Arduino](https://github.com/esp8266/Arduino#installing-with-boards-manager))
2. **Boardverwalter** öffnen, nach `esp8266` suchen und installieren (getestet mit 3.1.2).
3. Arduino IDE **komplett schließen und neu öffnen**, sonst kennt sie den neuen Core nicht.
4. **Werkzeuge → Board → esp8266 → NodeMCU 1.0 (ESP-12E Module)** auswählen und den COM-Port setzen. Unten rechts in der Statusleiste muss danach „NodeMCU 1.0 (ESP-12E Module)“ stehen.

Der ESP8266-Core bringt `ESP8266WiFi` und `ArduinoOTA` bereits mit. Diese Bibliotheken nicht zusätzlich über die Bibliotheksverwaltung installieren.

### Typische Fehler

| Fehlermeldung | Ursache | Lösung |
|---|---|---|
| `WiFiUdp.h: No such file or directory` | Falsches Board gewählt (z. B. Arduino Uno). Der Uno hat kein WLAN. | Board auf **NodeMCU 1.0 (ESP-12E Module)** umstellen. |
| `ESP8266WiFi.h: No such file or directory` | ESP8266-Core fehlt oder IDE wurde nach der Installation nicht neu gestartet. | Core installieren (siehe oben), IDE neu starten, Board wählen. |

### Bibliotheken (`libraries/`)

| Bibliothek | Version | Für | Hinweis |
|---|---|---|---|
| DHT sensor library (Adafruit) | 1.4.7 | Temperatursensor, LaptopKuehlung_Sensor | |
| Adafruit Unified Sensor | 1.1.15 | Temperatursensor, LaptopKuehlung_Sensor | Abhängigkeit der DHT sensor library |
| PubSubClient (Nick O'Leary) | 2.8 | LaptopKuehlung_Sensor, LaptopKuehlung_Luefter | MQTT-Client |

Alle drei sind über **Werkzeuge → Bibliotheken verwalten** installierbar. Der Ordner `libraries/` ist per `.gitignore` vom Repository ausgeschlossen und muss auf einem neuen Rechner neu installiert werden.

Reaktionszeit, AnalogeDatenauslesung und Wiederstandsmessen brauchen keine zusätzlichen Bibliotheken.

In `libraries/` sollen nur diese drei Bibliotheken liegen. Andere Bibliotheken werden von keinem Sketch verwendet und können entfernt werden.

## Laptop-Kühlung (2 Knoten über MQTT)

```
LaptopKuehlung_Sensor (NodeMCU + DHT22 + Ampel)    LaptopKuehlung_Luefter (NodeMCU + Lüfter)
   │  laptop/temperatur  "27.3"  ──► Broker ──►  AN ab 26.0 °C, AUS unter 25.0 °C
   └──────────── laptop/log ◄─────── Broker ◄────────┘
                       │
             Laptop: mosquitto_sub ► log.csv
```

### Verdrahtung

**LaptopKuehlung_Sensor:**

- DHT22-Modul: `+ → 3V3`, `OUT → D5`, `− → GND`. Ein nackter 4-Pin-DHT22 (ohne Platine) braucht zusätzlich 10 kΩ von DATA nach 3V3.
- LED-Ampel, jede LED mit ca. 220 Ω Vorwiderstand, kurzes Bein an GND: rot `D1`, gelb `D2`, grün `D6`.

| LED | Bedeutung |
|---|---|
| grün | Temperatur ≤ 25,0 °C |
| gelb | Temperatur > 25,0 °C |
| rot | Temperatur > 26,0 °C |
| rot blinkt | Sensorfehler (DHT22 liefert keinen Wert) |

**LaptopKuehlung_Luefter:**

```
VIN/VU (5 V) ──┬────────── Lüfter +
               │
         1N4007 (Ring zu +5 V)
               │
               ├────────── Lüfter −
               │
         PN2222 C
         PN2222 B ── 1 kΩ ── D6
         PN2222 E ────────── GND

D1 ── Taster ── GND
```

PN2222: flache Seite zu dir, Beine nach unten = E – B – C. Den Lüfter nie direkt an einen GPIO hängen (max. ca. 12 mA, 3,3 V).

### Zugangsdaten

In beiden Sketch-Ordnern `secrets.example.h` nach `secrets.h` kopieren und WLAN- sowie Broker-Daten eintragen. `secrets.h` steht in `.gitignore` und wird nicht committet.

### MQTT-Topics

Alle Topics beginnen mit `TOPIC_PREFIX` (Standard `laptop/`). Der Präfix steht oben in beiden Sketches und muss dort gleich sein.

| Topic | Von | Inhalt |
|---|---|---|
| `laptop/temperatur` | Sensor | Temperatur in °C, z. B. `27.3` (retained) |
| `laptop/sensor/status` | Sensor | `online` / `SENSOR_ERROR` / `offline` (retained, `offline` per Last Will) |
| `laptop/luefter/set` | du | `ON`, `OFF` oder `AUTO` |
| `laptop/luefter/status` | Lüfter | `ON` / `OFF` (retained) |
| `laptop/luefter/mode` | Lüfter | `AUTO` / `ON` / `OFF` (retained) |
| `laptop/luefter/online` | Lüfter | `online` / `offline` (retained, Last Will) |
| `laptop/log` | beide | Log-Zeilen (CSV) |

### Verhalten des Lüfters

- **AUTO** (Standard): AN ab `TEMP_ON` (26,0 °C), AUS unter `TEMP_OFF` (25,0 °C). Dazwischen bleibt der Zustand, damit der Lüfter nicht ständig schaltet.
- **Fail-safe**, der Lüfter geht AN:
  - sofort, wenn der Sensor `SENSOR_ERROR` oder `offline` meldet,
  - wenn 60 s keine Temperatur kommt (z. B. WLAN des Sensors weg).
- **Taster an D1:** wechselt zwischen AUTO und dauerhaft AN.
- **MQTT:** `laptop/luefter/set` mit `ON`, `OFF` oder `AUTO`.

Schwellen und Zeiten stehen oben im Sketch unter „Einstellungen“.

### Logging

Beide Knoten schreiben jede Log-Zeile in den Serial Monitor (115200 Baud) und nach `laptop/log`. Format (CSV, Trennzeichen `;`):

```
zeit;gerät;ereignis;wert;detail
2026-11-11 11:20:05;sensor;temp;27.3;hum=41%
2026-11-11 11:20:05;luefter;fan;ON;temp=27.3>=26.0
2026-11-11 11:22:40;sensor;sensor_status;SENSOR_ERROR;DHT22 liefert NaN, Verkabelung prüfen
2026-11-11 11:22:40;luefter;failsafe;ON;sensor=SENSOR_ERROR
```

Die Uhrzeit kommt per NTP aus dem Internet. Bis die Zeit synchronisiert ist, steht dort `uptime <n>s`.

Mitschreiben auf dem Laptop in eine Datei:

```bash
mosquitto_sub -h localhost -t laptop/log >> log.csv
```

### Broker (Mosquitto)

Mosquitto auf dem Laptop installieren ([mosquitto.org/download](https://mosquitto.org/download/)). Ab Version 2.0 nimmt Mosquitto ohne Konfiguration nur Verbindungen vom eigenen Rechner an ([Quelle](https://mosquitto.org/documentation/migrating-to-2-0/)). Damit die NodeMCUs sich verbinden können, in `mosquitto.conf` eintragen:

```
listener 1883
allow_anonymous true
```

Das ist nur für ein vertrauenswürdiges Heimnetz gedacht. In der Windows-Firewall muss Port 1883 freigegeben sein. In `secrets.h` als `MQTT_SERVER` die IP des Laptops eintragen (`ipconfig`).

Test ohne Sensor (nur Lüfter-Knoten angeschlossen):

| Befehl | Erwartung |
|---|---|
| `mosquitto_pub -h localhost -t laptop/sensor/status -m online` | Sensor gilt als ok |
| `mosquitto_pub -h localhost -t laptop/temperatur -m 27.5` | Lüfter AN |
| `mosquitto_pub -h localhost -t laptop/temperatur -m 24.0` | Lüfter AUS |
| `mosquitto_pub -h localhost -t laptop/sensor/status -m SENSOR_ERROR` | Lüfter sofort AN (Fail-safe) |

Ohne neue Temperatur geht der Lüfter nach 60 s ebenfalls in den Fail-safe.

Alles mitlesen:

```bash
mosquitto_sub -h localhost -t "laptop/#" -v
```
