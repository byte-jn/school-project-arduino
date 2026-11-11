/*
  LaptopKuehlung_Sensor – Sensor-Seite der Laptop-Kühlung (Arduino Uno)

  Misst Temperatur und Luftfeuchte und schickt sie per USB an die Laptop-Brücke
  (LaptopKuehlung_Bridge/bridge.py). Die Brücke publiziert per MQTT und bestätigt
  erst, wenn der Broker die Nachricht angenommen hat.

  Board:  Arduino Uno (AVR)
  Libs:   DHT sensor library (+ Adafruit Unified Sensor)

  Verdrahtung:
    DHT22-Modul:  + -> 5V,  OUT -> D2,  - -> GND
                  (nackter 4-Pin-Sensor: zusätzlich 10 kΩ von DATA nach 5V)
    LED-Ampel (je ca. 220 Ω Vorwiderstand, kurzes Bein an GND):
                  rot -> D13,  gelb -> D12,  grün -> D11
    USB zum Laptop (Brücke).

  USB-Leitung (115200 Baud): Log-Zeilen und Datenrahmen gemischt.
    Datenrahmen:  @<TYP>|<NR>|<key=wert&key=wert>*<PRUEFSUMME>
      TYP         H = Hallo, D = Daten, S = Status, A = Bestätigung, N = Ablehnung
      NR          laufende Nummer 1..65535; A/N wiederholen die Nummer der bestätigten Nachricht
      PRUEFSUMME  XOR aller Zeichen zwischen '@' und '*', zwei Hex-Ziffern
    Log-Zeilen:   zeit_ms;geraet;level;ereignis;key=wert,key=wert
      level: DEBUG (rohe Rahmen), INFO, WARN, ERROR

  Verbindungsstatus zur Brücke:
    unbekannt  noch nichts gesendet
    online     letzte Nachricht bestätigt
    unsicher   letzter Versuch fehlgeschlagen, es wird weiter versucht
    offline    MAX_FAILS Fehlschläge in Folge: bis zum Reset wird NICHT mehr gesendet,
               Messung, Log und Ampel laufen weiter
  Startet die Brücke neu, setzt sie den Uno beim Öffnen des Ports automatisch zurück.
*/

#include <DHT.h>

// ---------- Einstellungen ----------
const uint8_t DHT_PIN = 2;
const uint8_t DHT_TYPE = DHT22;
const uint8_t LED_RED = 13;
const uint8_t LED_YELLOW = 12;
const uint8_t LED_GREEN = 11;

const float WARN_TEMP = 25.0;    // Ampel: darüber gelb
const float BAD_TEMP  = 26.0;    // Ampel: darüber rot

const unsigned long MEASURE_INTERVAL_MS = 5000;   // DHT22 schafft höchstens alle 2 s eine Messung
const unsigned long STATS_INTERVAL_MS = 60000;    // Zusammenfassung im Log
const unsigned long BLINK_MS = 250;
const unsigned long ACK_TIMEOUT_MS = 1000;        // so lange auf die Bestätigung der Brücke warten
const uint8_t MAX_FAILS = 2;                      // Fehlschläge in Folge bis "offline"
const unsigned long CONNECT_RETRY_MS = 1000;      // Pause zwischen den Startversuchen
const bool LOG_FRAMES = true;                     // rohe Rahmen als DEBUG loggen
const long BAUD = 115200;
const uint8_t PROTOCOL_VERSION = 3;
// -----------------------------------

enum LinkState { LINK_UNKNOWN, LINK_ONLINE, LINK_UNSURE, LINK_OFFLINE };
enum SendResult { SEND_OK, SEND_NAK, SEND_TIMEOUT, SEND_SKIPPED };

DHT dht(DHT_PIN, DHT_TYPE);

unsigned long lastMeasure = 0;
unsigned long lastStats = 0;
bool sensorError = false;
float lastTemp = NAN;

LinkState linkState = LINK_UNKNOWN;
uint8_t failedInRow = 0;
uint16_t nextSeq = 1;

unsigned long cntTx = 0, cntOk = 0, cntTimeout = 0, cntNak = 0, cntSkipped = 0;

// =====================================================================
// Logging: zeit_ms;sensor;LEVEL;ereignis;key=wert,key=wert
// =====================================================================
bool kvFirst = true;

void logStart(const __FlashStringHelper* level, const __FlashStringHelper* event) {
  Serial.print(millis());
  Serial.print(F(";sensor;"));
  Serial.print(level);
  Serial.print(';');
  Serial.print(event);
  Serial.print(';');
  kvFirst = true;
}

void kvKey(const __FlashStringHelper* key) {
  if (!kvFirst) {
    Serial.print(',');
  }
  kvFirst = false;
  Serial.print(key);
  Serial.print('=');
}

void kv(const __FlashStringHelper* key, const char* value) { kvKey(key); Serial.print(value); }
void kv(const __FlashStringHelper* key, const __FlashStringHelper* value) { kvKey(key); Serial.print(value); }
void kv(const __FlashStringHelper* key, unsigned long value) { kvKey(key); Serial.print(value); }
void kvTemp(const __FlashStringHelper* key, float value) { kvKey(key); Serial.print(value, 1); }

void logEnd() {
  Serial.println();
}

// =====================================================================
// Rahmen
// =====================================================================
uint8_t checksum(const char* s, size_t len) {
  uint8_t cs = 0;
  for (size_t i = 0; i < len; i++) {
    cs ^= (uint8_t)s[i];
  }
  return cs;
}

// Baut "<typ>|<seq>|<payload>*<cs>" (ohne '@') in buf.
void buildFrame(char* buf, size_t size, char type, uint16_t seq, const char* payload) {
  snprintf(buf, size, "%c|%u|%s", type, seq, payload);
  size_t len = strlen(buf);
  snprintf(buf + len, size - len, "*%02X", checksum(buf, len));
}

// Zerlegt "<typ>|<seq>|<payload>*<cs>" (ohne '@').
// Gibt den Typ zurück oder 0 bei kaputtem Rahmen.
char parseFrame(char* body, uint16_t& seq, char*& payload) {
  char* star = strrchr(body, '*');
  if (star == nullptr || strlen(star) != 3) {
    return 0;
  }
  uint8_t expected = (uint8_t)strtoul(star + 1, nullptr, 16);
  if (checksum(body, star - body) != expected) {
    return 0;
  }
  *star = '\0';
  if (body[0] == '\0' || body[1] != '|') {
    return 0;
  }
  char* bar = strchr(body + 2, '|');
  if (bar == nullptr) {
    return 0;
  }
  *bar = '\0';
  seq = (uint16_t)strtoul(body + 2, nullptr, 10);
  payload = bar + 1;
  return body[0];
}

// =====================================================================
// Verbindung zur Brücke
// =====================================================================
const __FlashStringHelper* linkName() {
  switch (linkState) {
    case LINK_ONLINE:  return F("online");
    case LINK_UNSURE:  return F("unsicher");
    case LINK_OFFLINE: return F("offline");
    default:           return F("unbekannt");
  }
}

// Sendet einen Rahmen an die Brücke und wartet auf "A|<seq>" bzw. "N|<seq>".
// Loggt Status vorher und Ergebnis nachher. Ist die Verbindung offline, wird nicht gesendet.
SendResult sendToBridge(char type, const char* payload) {
  char typeStr[2] = { type, '\0' };

  if (linkState == LINK_OFFLINE) {
    cntSkipped++;
    logStart(F("WARN"), F("send_skipped"));
    kv(F("typ"), typeStr);
    kv(F("link"), linkName());
    logEnd();
    return SEND_SKIPPED;
  }

  uint16_t seq = nextSeq++;
  if (nextSeq == 0) {
    nextSeq = 1;
  }
  char frame[56];
  buildFrame(frame, sizeof(frame), type, seq, payload);

  logStart(F("INFO"), F("send"));
  kv(F("typ"), typeStr);
  kv(F("nr"), (unsigned long)seq);
  kv(F("link"), linkName());
  logEnd();
  if (LOG_FRAMES) {
    logStart(F("DEBUG"), F("tx"));
    kv(F("frame"), frame);
    logEnd();
  }

  Serial.print('@');
  Serial.println(frame);
  cntTx++;

  SendResult result = SEND_TIMEOUT;
  char line[48];
  char replyRaw[48];
  replyRaw[0] = '\0';
  char nakReason[24];
  nakReason[0] = '\0';
  uint8_t len = 0;
  unsigned long start = millis();
  while (result == SEND_TIMEOUT && millis() - start < ACK_TIMEOUT_MS) {
    if (Serial.available() == 0) {
      continue;
    }
    char c = Serial.read();
    if (c != '\n' && c != '\r') {
      if (len < sizeof(line) - 1) {
        line[len++] = c;
      }
      continue;
    }
    if (len == 0) {
      continue;
    }
    line[len] = '\0';
    len = 0;
    if (line[0] != '@') {
      continue;   // kein Rahmen
    }
    strncpy(replyRaw, line + 1, sizeof(replyRaw));
    replyRaw[sizeof(replyRaw) - 1] = '\0';

    uint16_t replySeq = 0;
    char* replyPayload = nullptr;
    char replyType = parseFrame(line + 1, replySeq, replyPayload);
    if (replySeq != seq) {
      continue;   // kaputt oder Antwort auf eine andere Nachricht
    }
    if (replyType == 'A') {
      result = SEND_OK;
    } else if (replyType == 'N') {
      result = SEND_NAK;
      strncpy(nakReason, replyPayload, sizeof(nakReason));
      nakReason[sizeof(nakReason) - 1] = '\0';
    }
  }
  unsigned long rtt = millis() - start;

  if (LOG_FRAMES && replyRaw[0] != '\0') {
    logStart(F("DEBUG"), F("rx"));
    kv(F("frame"), replyRaw);
    logEnd();
  }

  if (result == SEND_OK) {
    failedInRow = 0;
    linkState = LINK_ONLINE;
    cntOk++;
    logStart(F("INFO"), F("send_ok"));
  } else {
    failedInRow++;
    linkState = (failedInRow >= MAX_FAILS) ? LINK_OFFLINE : LINK_UNSURE;
    if (result == SEND_NAK) {
      cntNak++;
      logStart(F("WARN"), F("send_nak"));
    } else {
      cntTimeout++;
      logStart(F("WARN"), F("send_timeout"));
    }
  }
  kv(F("nr"), (unsigned long)seq);
  kv(F("rtt_ms"), rtt);
  kv(F("link"), linkName());
  if (nakReason[0] != '\0') {
    kv(F("antwort"), nakReason);
  }
  logEnd();

  if (linkState == LINK_OFFLINE) {
    logStart(F("ERROR"), F("link_offline"));
    kv(F("fehlschlaege"), (unsigned long)failedInRow);
    kv(F("hinweis"), F("es wird nicht mehr gesendet, Reset zum Neuversuch"));
    logEnd();
  }
  return result;
}

// =====================================================================
// Anzeige
// =====================================================================
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

// =====================================================================
// Sensor
// =====================================================================
void setSensorError(bool error) {
  if (error == sensorError) {
    return;
  }
  sensorError = error;
  if (error) {
    logStart(F("WARN"), F("sensor_error"));
    kv(F("hinweis"), F("DHT22 liefert NaN, Verkabelung pruefen"));
  } else {
    logStart(F("INFO"), F("sensor_ok"));
  }
  logEnd();
}

void measure() {
  float temp = dht.readTemperature();
  float hum = dht.readHumidity();

  if (isnan(temp)) {
    setSensorError(true);
    // Jedes Mal senden, damit auch ein neu gestarteter Lüfter-Uno den Fehler mitbekommt.
    sendToBridge('S', "sensor=error");
    return;
  }
  if (sensorError) {
    setSensorError(false);
    sendToBridge('S', "sensor=ok");
  }
  lastTemp = temp;
  showTemperature(temp);

  logStart(F("INFO"), F("measure"));
  kvTemp(F("temp"), temp);
  if (!isnan(hum)) {
    kv(F("hum"), (unsigned long)(hum + 0.5));
  }
  logEnd();

  char payload[32];
  char tempStr[8];
  dtostrf(temp, 1, 1, tempStr);
  if (isnan(hum)) {
    snprintf(payload, sizeof(payload), "temp=%s", tempStr);
  } else {
    snprintf(payload, sizeof(payload), "temp=%s&hum=%u", tempStr, (unsigned)(hum + 0.5));
  }
  sendToBridge('D', payload);
}

void logStats() {
  logStart(F("INFO"), F("stats"));
  kv(F("gesendet"), cntTx);
  kv(F("ok"), cntOk);
  kv(F("timeout"), cntTimeout);
  kv(F("abgelehnt"), cntNak);
  kv(F("uebersprungen"), cntSkipped);
  kv(F("link"), linkName());
  if (!isnan(lastTemp)) {
    kvTemp(F("temp"), lastTemp);
  }
  kv(F("sensor"), sensorError ? F("error") : F("ok"));
  logEnd();
}

// =====================================================================
// Start-Diagnose
// =====================================================================
void logBootInfo() {
  logStart(F("INFO"), F("boot"));
  kv(F("board"), F("uno"));
  kv(F("sketch"), F("LaptopKuehlung_Sensor"));
  kv(F("protokoll"), (unsigned long)PROTOCOL_VERSION);
  kv(F("baud"), (unsigned long)BAUD);
  logEnd();

  logStart(F("INFO"), F("config"));
  kv(F("dht22_pin"), (unsigned long)DHT_PIN);
  kv(F("led_rot"), (unsigned long)LED_RED);
  kv(F("led_gelb"), (unsigned long)LED_YELLOW);
  kv(F("led_gruen"), (unsigned long)LED_GREEN);
  kvTemp(F("gelb_ab"), WARN_TEMP);
  kvTemp(F("rot_ab"), BAD_TEMP);
  kv(F("intervall_ms"), MEASURE_INTERVAL_MS);
  kv(F("ack_timeout_ms"), ACK_TIMEOUT_MS);
  kv(F("max_fehlschlaege"), (unsigned long)MAX_FAILS);
  logEnd();
}

void testSensor() {
  delay(2000);   // DHT22 braucht nach dem Einschalten etwas Zeit
  float temp = dht.readTemperature();
  if (isnan(temp)) {
    logStart(F("WARN"), F("sensor_test"));
    kv(F("ergebnis"), F("fehler"));
    kv(F("hinweis"), F("DHT22 liefert NaN, Verkabelung pruefen"));
  } else {
    logStart(F("INFO"), F("sensor_test"));
    kv(F("ergebnis"), F("ok"));
    kvTemp(F("temp"), temp);
  }
  logEnd();
}

// Bis zu MAX_FAILS Hallo-Nachrichten an die Brücke. Antwortet sie nicht, ist die Verbindung offline.
void connectToBridge() {
  char payload[24];
  snprintf(payload, sizeof(payload), "dev=sensor&ver=%u", PROTOCOL_VERSION);

  for (uint8_t attempt = 1; attempt <= MAX_FAILS && linkState != LINK_OFFLINE; attempt++) {
    logStart(F("INFO"), F("connect"));
    kv(F("ziel"), F("bridge"));
    kv(F("versuch"), (unsigned long)attempt);
    kv(F("von"), (unsigned long)MAX_FAILS);
    logEnd();

    if (sendToBridge('H', payload) == SEND_OK) {
      logStart(F("INFO"), F("connect_ok"));
      kv(F("link"), linkName());
      logEnd();
      return;
    }
    if (linkState != LINK_OFFLINE) {
      delay(CONNECT_RETRY_MS);
    }
  }
}

void setup() {
  Serial.begin(BAUD);

  pinMode(LED_RED, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  setLeds(LOW, LOW, LOW);

  dht.begin();

  Serial.println(F("zeit_ms;geraet;level;ereignis;details"));
  logBootInfo();
  testSensor();
  connectToBridge();
  lastStats = millis();
}

void loop() {
  if (millis() - lastMeasure >= MEASURE_INTERVAL_MS) {
    lastMeasure = millis();
    measure();
  }

  if (millis() - lastStats >= STATS_INTERVAL_MS) {
    lastStats = millis();
    logStats();
  }

  // Sensorfehler -> rote LED blinkt
  if (sensorError) {
    bool on = (millis() / BLINK_MS) % 2;
    setLeds(on, LOW, LOW);
  }
}
