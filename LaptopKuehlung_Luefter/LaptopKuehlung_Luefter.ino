/*
  LaptopKuehlung_Luefter – Seite 2 der Laptop-Kühlung (Arduino Uno)

  Empfängt die Temperatur per Kabel vom Sensor-Uno (LaptopKuehlung_Sensor)
  und schaltet den Lüfter.

  Board:  Arduino Uno (AVR)
  Libs:   keine zusätzlichen

  Verdrahtung:
    Verbindung zum Sensor-Uno:
      D10 (Empfangen) <- D3 am Sensor-Uno
      GND             -> GND am Sensor-Uno
    Lüfter (5 V) über NPN-Transistor PN2222:
      5V -> Lüfter +,  Lüfter - -> Collector
      D9 -> 1 kΩ -> Base,  Emitter -> GND
      Diode 1N4007 parallel zum Lüfter, Ring zu 5V
    Taster: D7 -> Taster -> GND   (interner Pull-up)

  Verhalten:
    - AUTO: Lüfter AN ab TEMP_ON, AUS unter TEMP_OFF (dazwischen bleibt der Zustand)
    - Fail-safe, Lüfter geht AN:
        * sofort, wenn der Sensor-Uno "status;SENSOR_ERROR" schickt
        * wenn TEMP_TIMEOUT_MS lang keine Temperatur kommt (Kabel ab, Sensor-Uno aus)
    - Taster: wechselt zwischen AUTO und dauerhaft AN
    - Serial Monitor (9600 Baud, Zeilenende "Neue Zeile"): ON, OFF oder AUTO eintippen

  Log über USB (Serial Monitor, 9600 Baud), CSV mit Trennzeichen ;
    zeit_s;geraet;ereignis;wert;detail
*/

#include <SoftwareSerial.h>

// ---------- Einstellungen ----------
const uint8_t FAN_PIN = 9;
const uint8_t BUTTON_PIN = 7;
const uint8_t LINK_RX_PIN = 10;  // Kabel vom Sensor-Uno (D3)
const uint8_t LINK_TX_PIN = 11;  // nicht angeschlossen, SoftwareSerial braucht einen Pin

const float TEMP_ON  = 26.0;                       // ab hier Lüfter AN
const float TEMP_OFF = 25.0;                       // darunter Lüfter AUS
const unsigned long TEMP_TIMEOUT_MS = 60000;       // ohne neue Temperatur -> Fail-safe AN
const unsigned long CHECK_INTERVAL_MS = 1000;
const unsigned long DEBOUNCE_MS = 50;
const long BAUD = 9600;
// -----------------------------------

enum Mode { MODE_AUTO, MODE_ON, MODE_OFF };

SoftwareSerial link(LINK_RX_PIN, LINK_TX_PIN);

Mode mode = MODE_AUTO;
bool fanOn = false;
bool failsafe = false;

float lastTemp = NAN;
unsigned long lastTempAt = 0;
bool haveTemp = false;
bool sensorOk = true;   // unbekannt zählt als ok; dann greift nur der Timeout

unsigned long lastCheck = 0;

int buttonStable = HIGH;
int buttonLastRead = HIGH;
unsigned long buttonChangedAt = 0;

char linkBuf[24];
uint8_t linkLen = 0;
char cmdBuf[8];
uint8_t cmdLen = 0;

// ---------- Logging ----------
// Schreibt "zeit_s;luefter;ereignis;" – Wert und Detail hängt der Aufrufer an.
void logBegin(const __FlashStringHelper* event) {
  Serial.print(millis() / 1000);
  Serial.print(F(";luefter;"));
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

const __FlashStringHelper* modeName(Mode m) {
  switch (m) {
    case MODE_ON:  return F("ON");
    case MODE_OFF: return F("OFF");
    default:       return F("AUTO");
  }
}

// ---------- Lüfter ----------
// reason wird ins Log geschrieben; temp nur, wenn sie der Grund ist (sonst NAN).
void setFan(bool on, const __FlashStringHelper* reason, float temp) {
  if (on == fanOn) {
    return;
  }
  fanOn = on;
  digitalWrite(FAN_PIN, on ? HIGH : LOW);

  logBegin(F("fan"));
  Serial.print(on ? F("ON") : F("OFF"));
  Serial.print(';');
  Serial.print(reason);
  if (!isnan(temp)) {
    Serial.print(F(" temp="));
    Serial.print(temp, 1);
  }
  Serial.println();
}

// Entscheidet anhand von Modus, Sensorstatus und letzter Temperatur.
void updateFan() {
  if (mode == MODE_ON) {
    setFan(true, F("modus=ON"), NAN);
    return;
  }
  if (mode == MODE_OFF) {
    setFan(false, F("modus=OFF"), NAN);
    return;
  }

  // Beim Start zählt der Timeout ab Boot, danach ab der letzten Temperatur.
  unsigned long since = haveTemp ? lastTempAt : 0;
  bool timedOut = millis() - since > TEMP_TIMEOUT_MS;
  bool stale = !sensorOk || timedOut;

  if (stale != failsafe) {
    failsafe = stale;
    if (!sensorOk) {
      logSimple(F("failsafe"), F("ON"), F("sensor=SENSOR_ERROR"));
    } else if (timedOut) {
      logSimple(F("failsafe"), F("ON"), F("keine Temperatur seit 60s"));
    } else {
      logSimple(F("failsafe"), F("OFF"), F("Sensor ok, Temperatur aktuell"));
    }
  }

  if (stale) {
    setFan(true, F("failsafe"), NAN);
  } else if (!haveTemp) {
    // Noch kein Wert, Timeout aber nicht abgelaufen: Zustand behalten.
  } else if (lastTemp >= TEMP_ON) {
    setFan(true, F("temp>=26.0"), lastTemp);
  } else if (lastTemp < TEMP_OFF) {
    setFan(false, F("temp<25.0"), lastTemp);
  }
  // Zwischen TEMP_OFF und TEMP_ON bleibt der Zustand (Hysterese).
}

void setMode(Mode m, const __FlashStringHelper* source) {
  if (m == mode) {
    return;
  }
  mode = m;
  logSimple(F("mode"), modeName(mode), source);
  updateFan();
}

// ---------- Nachrichten vom Sensor-Uno ----------
void setSensorOk(bool ok) {
  if (ok == sensorOk) {
    return;
  }
  sensorOk = ok;
  logSimple(F("sensor_status"), ok ? F("online") : F("SENSOR_ERROR"), F(""));
}

void handleLinkLine(char* line) {
  char* sep = strchr(line, ';');
  if (sep == nullptr) {
    logBegin(F("error"));
    Serial.print(F("invalid_link;"));
    Serial.println(line);
    return;
  }
  *sep = '\0';
  const char* key = line;
  const char* value = sep + 1;

  if (strcmp(key, "temperatur") == 0) {
    char* end;
    double t = strtod(value, &end);
    if (end == value) {
      logBegin(F("error"));
      Serial.print(F("invalid_temp;"));
      Serial.println(value);
      return;
    }
    lastTemp = t;
    lastTempAt = millis();
    haveTemp = true;
    setSensorOk(true);   // eine gültige Temperatur heißt: Sensor ok
    updateFan();
  } else if (strcmp(key, "status") == 0) {
    setSensorOk(strcmp(value, "online") == 0);
    updateFan();         // bei SENSOR_ERROR sofort Fail-safe
  } else {
    logBegin(F("error"));
    Serial.print(F("invalid_link;"));
    Serial.println(key);
  }
}

// Liest Zeilen vom Sensor-Uno, ohne zu blockieren.
void handleLink() {
  while (link.available() > 0) {
    char c = link.read();
    if (c == '\n' || c == '\r') {
      if (linkLen > 0) {
        linkBuf[linkLen] = '\0';
        handleLinkLine(linkBuf);
        linkLen = 0;
      }
    } else if (linkLen < sizeof(linkBuf) - 1) {
      linkBuf[linkLen++] = c;
    }
  }
}

// ---------- Eingaben ----------
// Taster schaltet zwischen AUTO und dauerhaft AN.
void handleButton() {
  int reading = digitalRead(BUTTON_PIN);
  if (reading != buttonLastRead) {
    buttonLastRead = reading;
    buttonChangedAt = millis();
  }
  if (millis() - buttonChangedAt > DEBOUNCE_MS && reading != buttonStable) {
    buttonStable = reading;
    if (buttonStable == LOW) {  // gedrückt
      setMode(mode == MODE_AUTO ? MODE_ON : MODE_AUTO, F("taster"));
    }
  }
}

void handleCommand(const char* cmd) {
  if (strcasecmp(cmd, "ON") == 0) {
    setMode(MODE_ON, F("serial"));
  } else if (strcasecmp(cmd, "OFF") == 0) {
    setMode(MODE_OFF, F("serial"));
  } else if (strcasecmp(cmd, "AUTO") == 0) {
    setMode(MODE_AUTO, F("serial"));
  } else {
    logBegin(F("error"));
    Serial.print(F("invalid_command;"));
    Serial.println(cmd);
  }
}

// Liest Befehle aus dem Serial Monitor, ohne zu blockieren.
void handleSerial() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdLen > 0) {
        cmdBuf[cmdLen] = '\0';
        handleCommand(cmdBuf);
        cmdLen = 0;
      }
    } else if (cmdLen < sizeof(cmdBuf) - 1) {
      cmdBuf[cmdLen++] = c;
    }
  }
}

void setup() {
  Serial.begin(BAUD);
  link.begin(BAUD);

  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(FAN_PIN, LOW);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Serial.println(F("zeit_s;geraet;ereignis;wert;detail"));
  logSimple(F("start"), F("AUTO"), F("Befehle: ON, OFF, AUTO"));
}

void loop() {
  handleLink();
  handleSerial();
  handleButton();

  if (millis() - lastCheck >= CHECK_INTERVAL_MS) {
    lastCheck = millis();
    updateFan();
  }
}
