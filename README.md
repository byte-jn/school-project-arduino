# Arduino-Projekte

- [Reaktionszeit](Reaktionszeit/Reaktionszeit.txt): LED leuchtet nach 1–3 s zufällig auf, Taster stoppt die Zeit, Ausgabe in ms im Serial Monitor.
- [Temperatursensor](Temperatursensor/Temperatursensor.txt): DHT11 misst die Temperatur, Ampel-LEDs zeigen grün (bis 25 °C), gelb (über 25 °C) oder rot (über 26 °C).
- [AnalogeDatenauslesung](AnalogeDatenauslesung/AnalogeDatenauslesung.txt): Potentiometer an A3 wird ausgelesen, drei LEDs (rot, gelb, blau) zeigen je ein Drittel des Wertebereichs, Rohwert im Serial Monitor.

Benötigte Bibliotheken liegen in `libraries/` (DHT_sensor_library, Adafruit_Unified_Sensor).
