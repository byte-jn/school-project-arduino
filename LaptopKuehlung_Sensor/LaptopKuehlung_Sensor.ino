/*
  LaptopKuehlung_Sensor – Seite 1 der Laptop-Kühlung (Arduino Uno)

  Misst die Temperatur und schickt sie per Kabel (serielle Leitung) an den
  Lüfter-Uno (LaptopKuehlung_Luefter).

  Board:  Arduino Uno (AVR)
  Libs:   DHT sensor library (+ Adafruit Unified Sensor)

  Verdrahtung:
    DHT22-Modul:  + -> 5V,  OUT -> D2,  - -> GND
                  (nackter 4-Pin-Sensor: zusätzlich 10 kΩ von DATA nach 5V)
    LED-Ampel (je ca. 220 Ω Vorwiderstand, kurzes Bein an GND):
                  rot -> D13,  gelb -> D12,  grün -> D11
    Verbindung zum Lüfter-Uno:
                  D3 (Senden) -> D10 am Lüfter-Uno
                  GND         -> GND am Lüfter-Uno

  Anzeige:
    grün        Temperatur <= WARN_TEMP
    gelb        Temperatur >  WARN_TEMP
    rot         Temperatur >  BAD_TEMP
    rot blinkt  Sensorfehler

  Nachrichten an den Lüfter-Uno (eine Zeile pro Nachricht, 9600 Baud):
    temperatur;27.3       alle 5 s, wenn der Sensor einen Wert liefert
    status;SENSOR_ERROR   alle 5 s, solange der Sensor keinen Wert liefert
    status;online         einmal beim Start

  Log über USB (Serial Monitor, 9600 Baud), CSV mit Trennzeichen ;
    zeit_s;geraet;ereignis;wert;detail
*/

#include <DHT.h>
#include <SoftwareSerial.h>

// ---------- Einstellungen ----------
const uint8_t DHT_PIN = 2;
const uint8_t DHT_TYPE = DHT22;
const uint8_t LED_RED = 13;
const uint8_t LED_YELLOW = 12;
const uint8_t LED_GREEN = 11;
const uint8_t LINK_TX_PIN = 3;   // Kabel zum Lüfter-Uno (D10)
const uint8_t LINK_RX_PIN = 4;   // nicht angeschlossen, SoftwareSerial braucht einen Pin

const float WARN_TEMP = 25.0;    // darüber gelb
const float BAD_TEMP  = 26.0;    // darüber rot

const unsigned long MEASURE_INTERVAL_MS = 5000;   // DHT22 schafft höchstens alle 2 s eine Messung
const unsigned long BLINK_MS = 250;
const long BAUD = 9600;
// -----------------------------------

DHT dht(DHT_PIN, DHT_TYPE);
SoftwareSerial link(LINK_RX_PIN, LINK_TX_PIN);

unsigned long lastMeasure = 0;
bool sensorError = false;

// ---------- Logging ----------
// Schreibt "zeit_s;sensor;ereignis;" – Wert und Detail hängt der Aufrufer an.
void logBegin(const __FlashStringHelper* event) {
  Serial.print(millis() / 1000);
  Serial.print(F(";sensor;"));
  Serial.print(event);
  Serial.print(';');
}

void logSimple(const __FlashStringHelper* event, const __FlashStringHelper* value,
               const __FlashStringHelper* detail) {
  logBegin(event);
  Serial.print(value);
  Serial.print(';');
  Serial.println(detail);
}

// ---------- Anzeige ----------
void setLeds(bool red, bool yellow, bool green) {
  digitalWrite(LED_RED, red);
  digitalWrite(LED_YELLOW, yellow);
  digitalWrite(LED_GREEN, green);
}

void showTemperature(float temp) {
  if (temp > BAD_TEMP) {
    setLeds(HIGH, LOW, LOW);
  } else if (temp > WARN_TEMP) {
    setLeds(LOW, HIGH, LOW);
  } else {
    setLeds(LOW, LOW, HIGH);
  }
}

// ---------- Sensor ----------
void setSensorError(bool error) {
  if (error == sensorError) {
    return;
  }
  sensorError = error;
  if (error) {
    logSimple(F("sensor_status"), F("SENSOR_ERROR"), F("DHT22 liefert NaN, Verkabelung pruefen"));
  } else {
    logSimple(F("sensor_status"), F("online"), F("Sensor wieder ok"));
  }
}

void measure() {
  float temp = dht.readTemperature();
  float hum = dht.readHumidity();

  if (isnan(temp)) {
    setSensorError(true);
    // Jedes Mal senden, damit auch ein neu gestarteter Lüfter-Uno den Fehler mitbekommt.
    link.println(F("status;SENSOR_ERROR"));
    return;
  }
  setSensorError(false);
  showTemperature(temp);

  link.print(F("temperatur;"));
  link.println(temp, 1);

  logBegin(F("temp"));
  Serial.print(temp, 1);
  Serial.print(';');
  if (!isnan(hum)) {
    Serial.print(F("hum="));
    Serial.print(hum, 0);
    Serial.print('%');
  }
  Serial.println();
}

void setup() {
  Serial.begin(BAUD);
  link.begin(BAUD);

  pinMode(LED_RED, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  setLeds(LOW, LOW, LOW);

  dht.begin();

  Serial.println(F("zeit_s;geraet;ereignis;wert;detail"));
  logSimple(F("start"), F("online"), F(""));
  link.println(F("status;online"));
}

void loop() {
  if (millis() - lastMeasure >= MEASURE_INTERVAL_MS) {
    lastMeasure = millis();
    measure();
  }

  // Sensorfehler -> rote LED blinkt
  if (sensorError) {
    bool on = (millis() / BLINK_MS) % 2;
    setLeds(on, LOW, LOW);
  }
}
