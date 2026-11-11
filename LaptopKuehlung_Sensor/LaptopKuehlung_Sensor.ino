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
                  D3 (Senden)     -> D10 am Lüfter-Uno
                  D4 (Empfangen)  <- D11 am Lüfter-Uno   (Bestätigung "ack")
                  GND             -> GND am Lüfter-Uno

  Anzeige:
    grün        Temperatur <= WARN_TEMP
    gelb        Temperatur >  WARN_TEMP
    rot         Temperatur >  BAD_TEMP
    rot blinkt  Sensorfehler

  Nachrichten an den Lüfter-Uno (eine Zeile pro Nachricht, 9600 Baud):
    temperatur;27.3       alle 5 s, wenn der Sensor einen Wert liefert
    status;SENSOR_ERROR   alle 5 s, solange der Sensor keinen Wert liefert
    status;online         einmal beim Start
  Der Lüfter-Uno antwortet auf jede Nachricht mit "ack;<name>" (oder "nak;..." bei Unsinn).

  Start-Diagnose:
    - loggt Board, Pins, Verbindung, Ziel und Messintervall ("info"-Zeilen)
    - testet den Sensor einmal ("sensor_test")
    - bis zu 2 Verbindungsversuche zum Lüfter-Uno ("connect"). Antwortet er nicht,
      wird abgebrochen: Messung und Ampel laufen weiter, gesendet wird erst nach Reset wieder.

  Log über USB (Serial Monitor, 9600 Baud), CSV mit Trennzeichen ;
    zeit_s;geraet;ereignis;wert;detail
  Bei jedem Senden zwei Zeilen:
    12;sensor;send;temperatur=27.3;link=online        <- Verbindungsstatus vor dem Senden
    12;sensor;send_result;ok;18ms link=online         <- ok / abgelehnt / timeout
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
const uint8_t LINK_RX_PIN = 4;   // Kabel vom Lüfter-Uno (D11), Bestätigungen

const float WARN_TEMP = 25.0;    // darüber gelb
const float BAD_TEMP  = 26.0;    // darüber rot

const unsigned long MEASURE_INTERVAL_MS = 5000;   // DHT22 schafft höchstens alle 2 s eine Messung
const unsigned long BLINK_MS = 250;
const unsigned long ACK_TIMEOUT_MS = 200;        // so lange auf "ack" vom Lüfter-Uno warten
const uint8_t CONNECT_ATTEMPTS = 2;              // Verbindungsversuche beim Start, dann Abbruch
const unsigned long CONNECT_RETRY_MS = 1000;     // Pause zwischen den Versuchen
const long BAUD = 9600;
// -----------------------------------

enum LinkState { LINK_UNKNOWN, LINK_ONLINE, LINK_OFFLINE };
enum SendResult { SEND_OK, SEND_NAK, SEND_TIMEOUT };

DHT dht(DHT_PIN, DHT_TYPE);
SoftwareSerial link(LINK_RX_PIN, LINK_TX_PIN);

unsigned long lastMeasure = 0;
bool sensorError = false;
LinkState linkState = LINK_UNKNOWN;   // Ergebnis der letzten Übertragung
bool linkAborted = false;             // true: Lüfter-Uno beim Start nicht erreichbar, es wird nicht gesendet

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

// ---------- Verbindung zum Lüfter-Uno ----------
const __FlashStringHelper* linkName() {
  switch (linkState) {
    case LINK_ONLINE:  return F("online");
    case LINK_OFFLINE: return F("offline");
    default:           return F("unbekannt");
  }
}

// Sendet "key;value", wartet auf "ack;key" und loggt Status vorher und Ergebnis nachher.
SendResult sendToFan(const __FlashStringHelper* key, const char* value) {
  logBegin(F("send"));
  Serial.print(key);
  Serial.print('=');
  Serial.print(value);
  Serial.print(F(";link="));
  Serial.println(linkName());

  while (link.available() > 0) {   // alte Antworten verwerfen
    link.read();
  }
  link.print(key);
  link.print(';');
  link.println(value);

  SendResult result = SEND_TIMEOUT;
  char reply[24];
  uint8_t len = 0;
  unsigned long start = millis();
  while (result == SEND_TIMEOUT && millis() - start < ACK_TIMEOUT_MS) {
    if (link.available() == 0) {
      continue;
    }
    char c = link.read();
    if (c != '\n' && c != '\r') {
      if (len < sizeof(reply) - 1) {
        reply[len++] = c;
      }
      continue;
    }
    if (len == 0) {
      continue;
    }
    reply[len] = '\0';
    len = 0;
    if (strncmp(reply, "ack;", 4) == 0 && strcmp_P(reply + 4, (PGM_P)key) == 0) {
      result = SEND_OK;
    } else if (strncmp(reply, "nak;", 4) == 0) {
      result = SEND_NAK;
    }
    // andere Zeilen ignorieren und weiter warten
  }
  unsigned long ms = millis() - start;

  linkState = (result == SEND_OK) ? LINK_ONLINE : LINK_OFFLINE;

  logBegin(F("send_result"));
  if (result == SEND_OK) {
    Serial.print(F("ok"));
  } else if (result == SEND_NAK) {
    Serial.print(F("abgelehnt"));
  } else {
    Serial.print(F("timeout"));
  }
  Serial.print(';');
  Serial.print(ms);
  Serial.print(F("ms link="));
  Serial.println(linkName());
  return result;
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
    if (!linkAborted) {
      sendToFan(F("status"), "SENSOR_ERROR");
    }
    return;
  }
  setSensorError(false);
  showTemperature(temp);

  if (!linkAborted) {
    char value[8];
    dtostrf(temp, 1, 1, value);
    sendToFan(F("temperatur"), value);
  }

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

// ---------- Start-Diagnose ----------
void logInfo(const __FlashStringHelper* key) {
  logBegin(F("info"));
  Serial.print(key);
  Serial.print(';');
}

// Schreibt beim Start alles, was für die Fehlersuche wichtig ist.
void logStartInfo() {
  logInfo(F("board"));
  Serial.println(F("Arduino Uno, Sketch LaptopKuehlung_Sensor"));

  logInfo(F("sensor"));
  Serial.print(F("DHT22 an D"));
  Serial.println(DHT_PIN);

  logInfo(F("ampel"));
  Serial.print(F("rot D"));
  Serial.print(LED_RED);
  Serial.print(F(", gelb D"));
  Serial.print(LED_YELLOW);
  Serial.print(F(", gruen D"));
  Serial.print(LED_GREEN);
  Serial.print(F(", gelb ab "));
  Serial.print(WARN_TEMP, 1);
  Serial.print(F(", rot ab "));
  Serial.println(BAD_TEMP, 1);

  logInfo(F("verbindung"));
  Serial.print(F("Kabel, SoftwareSerial "));
  Serial.print(BAUD);
  Serial.print(F(" Baud, Senden D"));
  Serial.print(LINK_TX_PIN);
  Serial.print(F(" -> Luefter D10, Empfangen D"));
  Serial.print(LINK_RX_PIN);
  Serial.println(F(" <- Luefter D11"));

  logInfo(F("ziel"));
  Serial.print(F("LaptopKuehlung_Luefter, ack-Timeout "));
  Serial.print(ACK_TIMEOUT_MS);
  Serial.print(F(" ms, "));
  Serial.print(CONNECT_ATTEMPTS);
  Serial.println(F(" Verbindungsversuche"));

  logInfo(F("messung"));
  Serial.print(F("alle "));
  Serial.print(MEASURE_INTERVAL_MS / 1000);
  Serial.println(F(" s"));
}

// Erste Messung, damit gleich beim Start klar ist, ob der Sensor antwortet.
void testSensor() {
  delay(2000);   // DHT22 braucht nach dem Einschalten etwas Zeit
  float temp = dht.readTemperature();
  logBegin(F("sensor_test"));
  if (isnan(temp)) {
    Serial.println(F("FEHLER;DHT22 liefert NaN, Verkabelung pruefen"));
  } else {
    Serial.print(F("ok;"));
    Serial.print(temp, 1);
    Serial.println(F(" C"));
  }
}

// Bis zu CONNECT_ATTEMPTS Handshakes mit dem Lüfter-Uno.
// Antwortet er nicht, wird abgebrochen: Messung und Ampel laufen weiter, gesendet wird nicht mehr.
void connectToFan() {
  for (uint8_t attempt = 1; attempt <= CONNECT_ATTEMPTS; attempt++) {
    logBegin(F("connect"));
    Serial.print(F("versuch "));
    Serial.print(attempt);
    Serial.print('/');
    Serial.print(CONNECT_ATTEMPTS);
    Serial.println(F(";LaptopKuehlung_Luefter"));

    if (sendToFan(F("status"), "online") == SEND_OK) {
      logSimple(F("connect"), F("ok"), F("Luefter-Uno erreichbar"));
      return;
    }
    if (attempt < CONNECT_ATTEMPTS) {
      delay(CONNECT_RETRY_MS);
    }
  }
  linkAborted = true;
  logBegin(F("connect"));
  Serial.print(F("abgebrochen;Luefter-Uno nach "));
  Serial.print(CONNECT_ATTEMPTS);
  Serial.println(F(" Versuchen nicht erreichbar, es wird nicht mehr gesendet (Reset zum Neuversuch)"));
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
  logStartInfo();
  testSensor();
  connectToFan();
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
