/*
  LaptopKuehlung_Luefter – Lüfter-Seite der Laptop-Kühlung (Arduino Uno)

  Bekommt die Temperatur per USB von der Laptop-Brücke (LaptopKuehlung_Bridge/bridge.py),
  die sie per MQTT vom Sensor-Uno empfängt, und schaltet den Lüfter.

  Board:  Arduino Uno (AVR)
  Libs:   keine zusätzlichen

  Verdrahtung:
    Relais-Modul (High Level Trigger): IN -> D3, VCC -> 5V, GND -> GND
    12-V-Lüfter über den Relais-Kontakt (COM/NO) an eigener 12-V-Versorgung
    USB zum Laptop (Brücke).

  Verhalten:
    - AUTO: Lüfter AN ab TEMP_ON, AUS unter TEMP_OFF (dazwischen bleibt der Zustand)
    - Fail-safe, Lüfter geht AN:
        * sofort, wenn der Sensor-Status nicht "ok" ist (error, offline)
        * wenn TEMP_TIMEOUT_MS lang keine Temperatur kommt (Brücke/Sensor weg)
    - Modus über MQTT (laptop/luefter/set) oder Serial Monitor ohne Brücke:
      ON, OFF, AUTO, STATUS eintippen (115200 Baud, Zeilenende "Neue Zeile")

  USB-Leitung (115200 Baud): Log-Zeilen und Datenrahmen gemischt.
    Datenrahmen:  @<TYP>|<NR>|<key=wert&key=wert>*<PRUEFSUMME>
      von der Brücke:  H = Hallo, T = Temperatur, S = Sensorstatus, C = Befehl (mode=...)
      an die Brücke:   H = Hallo, S = Lüfterzustand (fan=..&mode=..&failsafe=..)
      beide Richtungen: A = Bestätigung, N = Ablehnung (grund=...)
      PRUEFSUMME  XOR aller Zeichen zwischen '@' und '*', zwei Hex-Ziffern
    Log-Zeilen:   zeit_ms;geraet;level;ereignis;key=wert,key=wert
*/

// ---------- Einstellungen ----------
const uint8_t FAN_PIN = 3;                         // Relais-Eingang (IN)
const bool FAN_ACTIVE_LOW = false;                 // Relais ist "High Level Trigger"; true bei "Low Level Trigger"

const float TEMP_ON  = 23.0;                       // ab hier Lüfter AN
const float TEMP_OFF = 22.0;                       // darunter Lüfter AUS
const unsigned long TEMP_TIMEOUT_MS = 60000;       // ohne neue Temperatur -> Fail-safe AN
const unsigned long CHECK_INTERVAL_MS = 1000;
const unsigned long STATS_INTERVAL_MS = 60000;     // Zusammenfassung im Log
const unsigned long ACK_TIMEOUT_MS = 1000;         // so lange auf die Bestätigung der Brücke warten
const uint8_t MAX_FAILS = 2;                       // Fehlschläge in Folge bis "offline"
const bool LOG_FRAMES = true;                      // rohe Rahmen als DEBUG loggen
const long BAUD = 115200;
const uint8_t PROTOCOL_VERSION = 3;
// -----------------------------------

enum Mode { MODE_AUTO, MODE_ON, MODE_OFF };
enum LinkState { LINK_UNKNOWN, LINK_ONLINE, LINK_UNSURE, LINK_OFFLINE };
enum SensorState { SENSOR_UNKNOWN, SENSOR_OK, SENSOR_ERROR, SENSOR_OFFLINE };

Mode mode = MODE_AUTO;
bool fanOn = false;
bool failsafe = false;

float lastTemp = NAN;
unsigned long lastTempAt = 0;
bool haveTemp = false;
SensorState sensorState = SENSOR_UNKNOWN;   // unbekannt zählt als ok; dann greift nur der Timeout

// Verbindung zur Brücke (für die Zustandsmeldungen)
LinkState linkState = LINK_UNKNOWN;
uint8_t failedInRow = 0;
uint16_t nextSeq = 1;
uint16_t pendingSeq = 0;            // 0 = keine Meldung unterwegs
unsigned long pendingAt = 0;
bool stateDirty = true;             // Zustand muss (erneut) gemeldet werden

// Nummern der Brücken-Rahmen
uint16_t lastBridgeSeq = 0;

unsigned long lastCheck = 0;
unsigned long lastStats = 0;

char lineBuf[64];
uint8_t lineLen = 0;

unsigned long cntRx = 0, cntBad = 0, cntLost = 0, cntDup = 0;
unsigned long cntTx = 0, cntOk = 0, cntTimeout = 0, cntNak = 0, cntSkipped = 0;

// =====================================================================
// Logging: zeit_ms;luefter;LEVEL;ereignis;key=wert,key=wert
// =====================================================================
bool kvFirst = true;

void logStart(const __FlashStringHelper* level, const __FlashStringHelper* event) {
  Serial.print(millis());
  Serial.print(F(";luefter;"));
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
// Namen
// =====================================================================
const __FlashStringHelper* modeName(Mode m) {
  switch (m) {
    case MODE_ON:  return F("ON");
    case MODE_OFF: return F("OFF");
    default:       return F("AUTO");
  }
}

const __FlashStringHelper* linkName() {
  switch (linkState) {
    case LINK_ONLINE:  return F("online");
    case LINK_UNSURE:  return F("unsicher");
    case LINK_OFFLINE: return F("offline");
    default:           return F("unbekannt");
  }
}

const __FlashStringHelper* sensorName() {
  switch (sensorState) {
    case SENSOR_OK:      return F("ok");
    case SENSOR_ERROR:   return F("error");
    case SENSOR_OFFLINE: return F("offline");
    default:             return F("unbekannt");
  }
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

void buildFrame(char* buf, size_t size, char type, uint16_t seq, const char* payload) {
  snprintf(buf, size, "%c|%u|%s", type, seq, payload);
  size_t len = strlen(buf);
  snprintf(buf + len, size - len, "*%02X", checksum(buf, len));
}

// Zerlegt "<typ>|<seq>|<payload>*<cs>" (ohne '@'). Gibt den Typ zurück oder 0 bei kaputtem Rahmen.
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

// Sucht "key=wert" im Payload (Paare mit '&' getrennt) und kopiert den Wert nach out.
bool getParam(const char* payload, const char* key, char* out, size_t size) {
  size_t keyLen = strlen(key);
  const char* p = payload;
  while (*p != '\0') {
    const char* end = strchr(p, '&');
    size_t pairLen = end ? (size_t)(end - p) : strlen(p);
    if (pairLen > keyLen && strncmp(p, key, keyLen) == 0 && p[keyLen] == '=') {
      size_t valLen = pairLen - keyLen - 1;
      if (valLen >= size) {
        valLen = size - 1;
      }
      memcpy(out, p + keyLen + 1, valLen);
      out[valLen] = '\0';
      return true;
    }
    if (end == nullptr) {
      break;
    }
    p = end + 1;
  }
  return false;
}

void writeFrame(char type, uint16_t seq, const char* payload) {
  char frame[64];
  buildFrame(frame, sizeof(frame), type, seq, payload);
  Serial.print('@');
  Serial.println(frame);
  if (LOG_FRAMES) {
    logStart(F("DEBUG"), F("tx"));
    kv(F("frame"), frame);
    logEnd();
  }
}

void reply(bool ok, uint16_t seq, const char* reason) {
  if (ok) {
    writeFrame('A', seq, "");
  } else {
    char payload[32];
    snprintf(payload, sizeof(payload), "grund=%s", reason);
    writeFrame('N', seq, payload);
  }
}

// =====================================================================
// Zustandsmeldung an die Brücke (asynchron, eine Meldung gleichzeitig)
// =====================================================================
void sendStateReport() {
  char typeStr[2] = { 'S', '\0' };
  if (linkState == LINK_OFFLINE) {
    cntSkipped++;
    stateDirty = false;
    logStart(F("WARN"), F("send_skipped"));
    kv(F("typ"), typeStr);
    kv(F("link"), linkName());
    logEnd();
    return;
  }

  char payload[48];
  snprintf(payload, sizeof(payload), "fan=%s&mode=%s&failsafe=%u",
           fanOn ? "ON" : "OFF",
           mode == MODE_ON ? "ON" : (mode == MODE_OFF ? "OFF" : "AUTO"),
           failsafe ? 1 : 0);

  uint16_t seq = nextSeq++;
  if (nextSeq == 0) {
    nextSeq = 1;
  }
  logStart(F("INFO"), F("send"));
  kv(F("typ"), typeStr);
  kv(F("nr"), (unsigned long)seq);
  kv(F("link"), linkName());
  logEnd();

  writeFrame('S', seq, payload);
  cntTx++;
  pendingSeq = seq;
  pendingAt = millis();
  stateDirty = false;
}

void finishPending(bool ok, bool nak, const char* reason) {
  unsigned long rtt = millis() - pendingAt;
  uint16_t seq = pendingSeq;
  pendingSeq = 0;

  if (ok) {
    failedInRow = 0;
    linkState = LINK_ONLINE;
    cntOk++;
    logStart(F("INFO"), F("send_ok"));
  } else {
    failedInRow++;
    linkState = (failedInRow >= MAX_FAILS) ? LINK_OFFLINE : LINK_UNSURE;
    stateDirty = true;   // Zustand nochmal melden (wird bei offline übersprungen)
    if (nak) {
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
  if (reason != nullptr && reason[0] != '\0') {
    kv(F("antwort"), reason);
  }
  logEnd();

  if (linkState == LINK_OFFLINE) {
    logStart(F("ERROR"), F("link_offline"));
    kv(F("fehlschlaege"), (unsigned long)failedInRow);
    kv(F("hinweis"), F("Zustand wird nicht mehr gemeldet, Luefter regelt weiter"));
    logEnd();
  }
}

void handleReports() {
  if (pendingSeq != 0 && millis() - pendingAt > ACK_TIMEOUT_MS) {
    finishPending(false, false, nullptr);
  }
  if (pendingSeq == 0 && stateDirty) {
    sendStateReport();
  }
}

// =====================================================================
// Lüfter
// =====================================================================
void writeFanPin(bool on) {
  digitalWrite(FAN_PIN, (on != FAN_ACTIVE_LOW) ? HIGH : LOW);
}

void setFan(bool on, const __FlashStringHelper* reason) {
  if (on == fanOn) {
    return;
  }
  fanOn = on;
  writeFanPin(on);
  stateDirty = true;

  logStart(F("INFO"), F("fan"));
  kv(F("zustand"), on ? F("ON") : F("OFF"));
  kv(F("grund"), reason);
  if (!isnan(lastTemp)) {
    kvTemp(F("temp"), lastTemp);
  }
  kvTemp(F("an_ab"), TEMP_ON);
  kvTemp(F("aus_unter"), TEMP_OFF);
  logEnd();
}

// Entscheidet anhand von Modus, Sensorstatus und letzter Temperatur.
void updateFan() {
  if (mode == MODE_ON) {
    setFan(true, F("modus_on"));
    return;
  }
  if (mode == MODE_OFF) {
    setFan(false, F("modus_off"));
    return;
  }

  // Beim Start zählt der Timeout ab Boot, danach ab der letzten Temperatur.
  unsigned long since = haveTemp ? lastTempAt : 0;
  bool timedOut = millis() - since > TEMP_TIMEOUT_MS;
  bool sensorBad = (sensorState == SENSOR_ERROR || sensorState == SENSOR_OFFLINE);
  bool stale = sensorBad || timedOut;

  if (stale != failsafe) {
    failsafe = stale;
    stateDirty = true;
    if (stale) {
      logStart(F("WARN"), F("failsafe"));
      kv(F("zustand"), F("ON"));
      if (sensorBad) {
        kv(F("grund"), F("sensor"));
        kv(F("sensor"), sensorName());
      } else {
        kv(F("grund"), F("timeout"));
        kv(F("ohne_temp_s"), TEMP_TIMEOUT_MS / 1000);
      }
    } else {
      logStart(F("INFO"), F("failsafe"));
      kv(F("zustand"), F("OFF"));
      kv(F("grund"), F("temperatur_aktuell"));
    }
    logEnd();
  }

  if (stale) {
    setFan(true, F("failsafe"));
  } else if (!haveTemp) {
    // Noch kein Wert, Timeout aber nicht abgelaufen: Zustand behalten.
  } else if (lastTemp >= TEMP_ON) {
    setFan(true, F("temp_hoch"));
  } else if (lastTemp < TEMP_OFF) {
    setFan(false, F("temp_niedrig"));
  }
  // Zwischen TEMP_OFF und TEMP_ON bleibt der Zustand (Hysterese).
}

void setMode(Mode m, const __FlashStringHelper* source) {
  if (m == mode) {
    return;
  }
  mode = m;
  stateDirty = true;
  logStart(F("INFO"), F("mode"));
  kv(F("modus"), modeName(mode));
  kv(F("quelle"), source);
  logEnd();
  updateFan();
}

bool parseMode(const char* s, Mode& m) {
  if (strcasecmp(s, "ON") == 0)   { m = MODE_ON;   return true; }
  if (strcasecmp(s, "OFF") == 0)  { m = MODE_OFF;  return true; }
  if (strcasecmp(s, "AUTO") == 0) { m = MODE_AUTO; return true; }
  return false;
}

// =====================================================================
// Eingehende Rahmen von der Brücke
// =====================================================================
void logBadFrame(const __FlashStringHelper* reason, const char* raw) {
  cntBad++;
  logStart(F("ERROR"), F("bad_frame"));
  kv(F("grund"), reason);
  kv(F("frame"), raw);
  logEnd();
}

// Prüft die laufende Nummer der Brücke. Gibt false zurück bei einem Duplikat.
bool checkBridgeSeq(char type, uint16_t seq) {
  if (type == 'H' || lastBridgeSeq == 0) {
    lastBridgeSeq = seq;
    return true;
  }
  if (seq == lastBridgeSeq) {
    cntDup++;
    logStart(F("WARN"), F("duplikat"));
    kv(F("nr"), (unsigned long)seq);
    logEnd();
    return false;
  }
  uint16_t expected = lastBridgeSeq + 1;
  if (expected == 0) {
    expected = 1;
  }
  if (seq != expected) {
    uint16_t lost = seq - expected;
    cntLost += lost;
    logStart(F("WARN"), F("nr_luecke"));
    kv(F("erwartet"), (unsigned long)expected);
    kv(F("bekommen"), (unsigned long)seq);
    kv(F("verloren"), (unsigned long)lost);
    logEnd();
  }
  lastBridgeSeq = seq;
  return true;
}

void handleFrame(char* body) {
  char raw[64];
  strncpy(raw, body, sizeof(raw));
  raw[sizeof(raw) - 1] = '\0';
  if (LOG_FRAMES) {
    logStart(F("DEBUG"), F("rx"));
    kv(F("frame"), raw);
    logEnd();
  }

  uint16_t seq = 0;
  char* payload = nullptr;
  char type = parseFrame(body, seq, payload);
  if (type == 0) {
    reply(false, seq, "checksum");
    logBadFrame(F("checksum_oder_format"), raw);
    return;
  }

  // Antworten auf unsere Zustandsmeldung
  if (type == 'A' || type == 'N') {
    if (pendingSeq != 0 && seq == pendingSeq) {
      char reason[24] = "";
      if (type == 'N') {
        getParam(payload, "grund", reason, sizeof(reason));
      }
      finishPending(type == 'A', type == 'N', reason);
    }
    return;
  }

  cntRx++;
  if (!checkBridgeSeq(type, seq)) {
    reply(true, seq, "");   // Duplikat: nochmal bestätigen, aber nicht erneut verarbeiten
    return;
  }

  char value[16];
  switch (type) {
    case 'H': {
      reply(true, seq, "");
      logStart(F("INFO"), F("hallo"));
      kv(F("von"), getParam(payload, "dev", value, sizeof(value)) ? value : "?");
      logEnd();
      stateDirty = true;   // Brücke kennt unseren Zustand noch nicht
      return;
    }
    case 'T': {
      if (!getParam(payload, "temp", value, sizeof(value))) {
        reply(false, seq, "temp_fehlt");
        logBadFrame(F("temp_fehlt"), raw);
        return;
      }
      char* end;
      double t = strtod(value, &end);
      if (end == value) {
        reply(false, seq, "temp_ungueltig");
        logBadFrame(F("temp_ungueltig"), raw);
        return;
      }
      reply(true, seq, "");
      lastTemp = t;
      lastTempAt = millis();
      haveTemp = true;
      if (sensorState != SENSOR_OK) {
        sensorState = SENSOR_OK;   // eine gültige Temperatur heißt: Sensor ok
        logStart(F("INFO"), F("sensor_status"));
        kv(F("sensor"), sensorName());
        kv(F("grund"), F("temperatur_empfangen"));
        logEnd();
      }
      logStart(F("INFO"), F("temp"));
      kv(F("nr"), (unsigned long)seq);
      kvTemp(F("temp"), lastTemp);
      if (getParam(payload, "hum", value, sizeof(value))) {
        kv(F("hum"), value);
      }
      logEnd();
      updateFan();
      return;
    }
    case 'S': {
      if (!getParam(payload, "sensor", value, sizeof(value))) {
        reply(false, seq, "sensor_fehlt");
        logBadFrame(F("sensor_fehlt"), raw);
        return;
      }
      reply(true, seq, "");
      SensorState s = SENSOR_UNKNOWN;
      if (strcmp(value, "ok") == 0) {
        s = SENSOR_OK;
      } else if (strcmp(value, "error") == 0) {
        s = SENSOR_ERROR;
      } else if (strcmp(value, "offline") == 0) {
        s = SENSOR_OFFLINE;
      }
      if (s != sensorState) {
        sensorState = s;
        logStart(s == SENSOR_OK ? F("INFO") : F("WARN"), F("sensor_status"));
        kv(F("sensor"), sensorName());
        logEnd();
      }
      updateFan();   // bei error/offline sofort Fail-safe
      return;
    }
    case 'C': {
      Mode m;
      if (!getParam(payload, "mode", value, sizeof(value)) || !parseMode(value, m)) {
        reply(false, seq, "befehl_ungueltig");
        logBadFrame(F("befehl_ungueltig"), raw);
        return;
      }
      reply(true, seq, "");
      setMode(m, F("mqtt"));
      stateDirty = true;
      return;
    }
    default:
      reply(false, seq, "typ_unbekannt");
      logBadFrame(F("typ_unbekannt"), raw);
      return;
  }
}

// =====================================================================
// Eingaben
// =====================================================================
void logStats() {
  logStart(F("INFO"), F("stats"));
  kv(F("empfangen"), cntRx);
  kv(F("fehlerhaft"), cntBad);
  kv(F("verloren"), cntLost);
  kv(F("duplikate"), cntDup);
  kv(F("gemeldet"), cntTx);
  kv(F("bestaetigt"), cntOk);
  kv(F("timeout"), cntTimeout);
  kv(F("link"), linkName());
  kv(F("luefter"), fanOn ? F("ON") : F("OFF"));
  kv(F("modus"), modeName(mode));
  kv(F("failsafe"), failsafe ? F("ON") : F("OFF"));
  kv(F("sensor"), sensorName());
  if (haveTemp) {
    kvTemp(F("temp"), lastTemp);
    kv(F("temp_alter_s"), (millis() - lastTempAt) / 1000);
  }
  logEnd();
}

// Textbefehle aus dem Serial Monitor (wenn keine Brücke läuft).
void handleCommand(const char* cmd) {
  Mode m;
  if (parseMode(cmd, m)) {
    setMode(m, F("serial"));
  } else if (strcasecmp(cmd, "STATUS") == 0) {
    logStats();
  } else {
    logStart(F("WARN"), F("invalid_command"));
    kv(F("befehl"), cmd);
    logEnd();
  }
}

// Liest Zeilen von USB: '@...' sind Rahmen, alles andere Textbefehle.
void handleSerial() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineLen > 0) {
        lineBuf[lineLen] = '\0';
        if (lineBuf[0] == '@') {
          handleFrame(lineBuf + 1);
        } else {
          handleCommand(lineBuf);
        }
        lineLen = 0;
      }
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    }
  }
}

// =====================================================================
// Start
// =====================================================================
void logBootInfo() {
  logStart(F("INFO"), F("boot"));
  kv(F("board"), F("uno"));
  kv(F("sketch"), F("LaptopKuehlung_Luefter"));
  kv(F("protokoll"), (unsigned long)PROTOCOL_VERSION);
  kv(F("baud"), (unsigned long)BAUD);
  logEnd();

  logStart(F("INFO"), F("config"));
  kv(F("luefter_pin"), (unsigned long)FAN_PIN);
  kv(F("aktiv"), FAN_ACTIVE_LOW ? F("LOW") : F("HIGH"));
  kvTemp(F("an_ab"), TEMP_ON);
  kvTemp(F("aus_unter"), TEMP_OFF);
  kv(F("failsafe_nach_s"), TEMP_TIMEOUT_MS / 1000);
  kv(F("ack_timeout_ms"), ACK_TIMEOUT_MS);
  kv(F("max_fehlschlaege"), (unsigned long)MAX_FAILS);
  logEnd();
}

void setup() {
  Serial.begin(BAUD);

  writeFanPin(false);          // erst Pegel setzen, dann Ausgang: Relais zuckt beim Start nicht
  pinMode(FAN_PIN, OUTPUT);

  Serial.println(F("zeit_ms;geraet;level;ereignis;details"));
  logBootInfo();

  char payload[24];
  snprintf(payload, sizeof(payload), "dev=luefter&ver=%u", PROTOCOL_VERSION);
  writeFrame('H', 0, payload);   // Hallo ohne Bestätigung; die Brücke erkennt daran die Rolle

  lastStats = millis();
}

void loop() {
  handleSerial();
  handleReports();

  if (millis() - lastCheck >= CHECK_INTERVAL_MS) {
    lastCheck = millis();
    updateFan();
  }

  if (millis() - lastStats >= STATS_INTERVAL_MS) {
    lastStats = millis();
    logStats();
  }
}
