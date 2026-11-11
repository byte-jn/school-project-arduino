# Arduino-Projekte

## Projekte

| Projekt | Board | Beschreibung |
|---|---|---|
| [Reaktionszeit](Reaktionszeit/Reaktionszeit.txt) | Arduino Uno (AVR) | LED leuchtet nach 1–3 s zufällig auf, Taster stoppt die Zeit, Ausgabe in ms im Serial Monitor. |
| [Temperatursensor](Temperatursensor/Temperatursensor.txt) | Arduino Uno (AVR) | DHT11 misst die Temperatur, Ampel-LEDs zeigen grün (bis 25 °C), gelb (über 25 °C) oder rot (über 26 °C). |
| [AnalogeDatenauslesung](AnalogeDatenauslesung/AnalogeDatenauslesung.txt) | Arduino Uno (AVR) | Potentiometer an A3 wird ausgelesen, drei LEDs (rot, gelb, blau) zeigen je ein Drittel des Wertebereichs, Rohwert im Serial Monitor. |
| [Wiederstandsmessen](Wiederstandsmessen/Wiederstandsmessen.txt) | Arduino Uno (AVR) | Platzhalter: schaltet bisher nur die eingebaute LED ein. |
| [LaptopKuehlungProjekt](LaptopKuehlungProjekt/LaptopKuehlungProjekt.ino) | NodeMCU 1.0 (ESP8266) | Lüfter an GPIO12 (D6) wird per MQTT (`esp8266/intopic`, `ON`/`OFF`) geschaltet, Taster an GPIO5 (D1) sendet `ON`/`OFF`, Updates per OTA. |

## Dependencies

### Board-Cores (Boardverwalter)

| Core | Version | Für | Installation |
|---|---|---|---|
| Arduino AVR Boards | 1.8.8 | Reaktionszeit, Temperatursensor, AnalogeDatenauslesung, Wiederstandsmessen | Bei der Arduino IDE dabei |
| esp8266 by ESP8266 Community | aktuell | LaptopKuehlungProjekt | Siehe unten |

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
| DHT sensor library (Adafruit) | 1.4.7 | Temperatursensor | |
| Adafruit Unified Sensor | 1.1.15 | Temperatursensor | Abhängigkeit der DHT sensor library |
| PubSubClient (Nick O'Leary) | 2.8 | LaptopKuehlungProjekt | MQTT-Client |

Alle drei sind über **Werkzeuge → Bibliotheken verwalten** installierbar. Der Ordner `libraries/` ist per `.gitignore` vom Repository ausgeschlossen und muss auf einem neuen Rechner neu installiert werden.

Reaktionszeit, AnalogeDatenauslesung und Wiederstandsmessen brauchen keine zusätzlichen Bibliotheken.

In `libraries/` sollen nur diese drei Bibliotheken liegen. Andere Bibliotheken werden von keinem Sketch verwendet und können entfernt werden.

### Konfiguration LaptopKuehlungProjekt

Vor dem Hochladen in [LaptopKuehlungProjekt.ino](LaptopKuehlungProjekt/LaptopKuehlungProjekt.ino) eintragen:

- `wifi_ssid`, `wifi_password`: WLAN-Zugangsdaten
- `mqtt_server`, `mqtt_user`, `mqtt_password`: MQTT-Broker (Port 1883)

Zugangsdaten nicht committen.
