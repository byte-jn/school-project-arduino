/*
  LaptopKuehlungProjekt – Knoten 2 der Laptop-Kühlung (Lüfter)

  Board:  NodeMCU 1.0 (ESP-12E Module), ESP8266-Core 3.1.2
  Libs:   PubSubClient

  Verdrahtung:
    Lüfter (5 V) über NPN-Transistor PN2222 an D6 (GPIO12):
      VIN/VU (5 V) -> Lüfter +      Lüfter - -> Collector
      D6 -> 1 kΩ -> Base            Emitter -> GND
      Diode 1N4007 parallel zum Lüfter, Ring zu +5 V
    Taster: D1 (GPIO5) -> Taster -> GND   (interner Pull-up)

  Funktion:
    - empfängt die Temperatur über  <prefix>temperatur
    - AUTO:  Lüfter AN ab TEMP_ON, AUS unter TEMP_OFF (dazwischen bleibt der Zustand)
    - Fail-safe, Lüfter geht AN:
        * sofort, wenn  <prefix>sensor/status  nicht "online" ist (SENSOR_ERROR, offline)
        * wenn TEMP_TIMEOUT_MS lang keine Temperatur kommt
    - Modus über  <prefix>luefter/set  ("ON", "OFF", "AUTO") oder Taster (AUTO <-> ON)
    - meldet  <prefix>luefter/status  (ON/OFF) und  <prefix>luefter/mode  (retained)
    - schreibt Zustandswechsel als CSV-Zeile nach Serial und  <prefix>log

  Zugangsdaten: secrets.example.h nach secrets.h kopieren und ausfüllen.
*/

#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <time.h>
#include "secrets.h"

// ---------- Einstellungen ----------
// Muss in beiden Sketches gleich sein.
#define TOPIC_PREFIX "laptop/"

const char* DEVICE_NAME = "luefter";
const uint8_t FAN_PIN = D6;
const uint8_t BUTTON_PIN = D1;

const float TEMP_ON  = 26.0;                       // ab hier Lüfter AN
const float TEMP_OFF = 25.0;                       // darunter Lüfter AUS
const unsigned long TEMP_TIMEOUT_MS = 60000;       // ohne neue Temperatur -> Fail-safe AN
const unsigned long CHECK_INTERVAL_MS = 1000;
const unsigned long MQTT_RETRY_MS = 5000;
const unsigned long DEBOUNCE_MS = 50;

const char* TOPIC_TEMP          = TOPIC_PREFIX "temperatur";
const char* TOPIC_SENSOR_STATUS = TOPIC_PREFIX "sensor/status";
const char* TOPIC_SET           = TOPIC_PREFIX "luefter/set";
const char* TOPIC_FAN_STATUS    = TOPIC_PREFIX "luefter/status";
const char* TOPIC_MODE          = TOPIC_PREFIX "luefter/mode";
const char* TOPIC_ONLINE        = TOPIC_PREFIX "luefter/online";
const char* TOPIC_LOG           = TOPIC_PREFIX "log";

const char* NTP_TZ = "CET-1CEST,M3.5.0,M10.5.0/3";  // Mitteleuropa inkl. Sommerzeit
// -----------------------------------

enum Mode { MODE_AUTO, MODE_ON, MODE_OFF };

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

Mode mode = MODE_AUTO;
bool fanOn = false;
bool failsafe = false;

float lastTemp = NAN;
unsigned long lastTempAt = 0;
bool haveTemp = false;

// Letzter Status des Sensors. Unbekannt zählt als ok; dann greift nur der Timeout.
String sensorStatus = "unbekannt";
bool sensorOk = true;

unsigned long lastCheck = 0;
unsigned long lastMqttAttempt = 0;

int buttonStable = HIGH;
int buttonLastRead = HIGH;
unsigned long buttonChangedAt = 0;

const char* modeName(Mode m) {
  switch (m) {
    case MODE_ON:  return "ON";
    case MODE_OFF: return "OFF";
    default:       return "AUTO";
  }
}

// Zeitstempel "YYYY-MM-DD HH:MM:SS"; ohne NTP-Zeit die Laufzeit seit dem Start.
String timestamp() {
  time_t now = time(nullptr);
  if (now < 1700000000) {
    return "uptime " + String(millis() / 1000) + "s";
  }
  struct tm t;
  localtime_r(&now, &t);
  char buf[20];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  return String(buf);
}

// Eine CSV-Zeile: zeit;gerät;ereignis;wert;detail
void logEvent(const char* event, const String& value, const String& detail = "") {
  String line = timestamp() + ";" + DEVICE_NAME + ";" + event + ";" + value + ";" + detail;
  Serial.println(line);
  if (mqtt.connected()) {
    mqtt.publish(TOPIC_LOG, line.c_str());
  }
}

void publishState() {
  mqtt.publish(TOPIC_FAN_STATUS, fanOn ? "ON" : "OFF", true);
  mqtt.publish(TOPIC_MODE, modeName(mode), true);
}

void setFan(bool on, const String& reason) {
  if (on == fanOn) {
    return;
  }
  fanOn = on;
  digitalWrite(FAN_PIN, on ? HIGH : LOW);
  publishState();
  logEvent("fan", on ? "ON" : "OFF", reason);
}

// Entscheidet anhand von Modus und letzter Temperatur, ob der Lüfter laufen soll.
void updateFan() {
  if (mode == MODE_ON) {
    setFan(true, "modus=ON");
    return;
  }
  if (mode == MODE_OFF) {
    setFan(false, "modus=OFF");
    return;
  }

  // Beim Start zählt der Timeout ab Boot, danach ab der letzten Temperatur.
  unsigned long since = haveTemp ? lastTempAt : 0;
  bool timedOut = millis() - since > TEMP_TIMEOUT_MS;
  bool stale = !sensorOk || timedOut;

  if (stale != failsafe) {
    failsafe = stale;
    String reason;
    if (!sensorOk) {
      reason = "sensor=" + sensorStatus;
    } else if (timedOut) {
      reason = "keine Temperatur seit " + String(TEMP_TIMEOUT_MS / 1000) + "s";
    } else {
      reason = "Sensor ok, Temperatur aktuell";
    }
    logEvent("failsafe", stale ? "ON" : "OFF", reason);
  }

  if (stale) {
    setFan(true, "failsafe");
  } else if (!haveTemp) {
    // Noch kein Wert, Timeout aber nicht abgelaufen: Zustand behalten.
  } else if (lastTemp >= TEMP_ON) {
    setFan(true, "temp=" + String(lastTemp, 1) + ">=" + String(TEMP_ON, 1));
  } else if (lastTemp < TEMP_OFF) {
    setFan(false, "temp=" + String(lastTemp, 1) + "<" + String(TEMP_OFF, 1));
  }
  // Zwischen TEMP_OFF und TEMP_ON bleibt der Zustand (Hysterese).
}

void setMode(Mode m, const char* source) {
  if (m == mode) {
    return;
  }
  mode = m;
  mqtt.publish(TOPIC_MODE, modeName(mode), true);
  logEvent("mode", modeName(mode), source);
  updateFan();
}

void onMessage(char* topic, byte* payload, unsigned int length) {
  char msg[16];
  unsigned int n = length < sizeof(msg) - 1 ? length : sizeof(msg) - 1;
  memcpy(msg, payload, n);
  msg[n] = '\0';

  if (strcmp(topic, TOPIC_TEMP) == 0) {
    char* end;
    float t = strtof(msg, &end);
    if (n == 0 || end == msg) {
      logEvent("error", "invalid_temp", String(msg));
      return;
    }
    lastTemp = t;
    lastTempAt = millis();
    haveTemp = true;
    updateFan();
  } else if (strcmp(topic, TOPIC_SET) == 0) {
    if (strcmp(msg, "ON") == 0)        setMode(MODE_ON, "mqtt");
    else if (strcmp(msg, "OFF") == 0)  setMode(MODE_OFF, "mqtt");
    else if (strcmp(msg, "AUTO") == 0) setMode(MODE_AUTO, "mqtt");
    else logEvent("error", "invalid_command", String(msg));
  } else if (strcmp(topic, TOPIC_SENSOR_STATUS) == 0) {
    sensorStatus = String(msg);
    sensorOk = (strcmp(msg, "online") == 0);
    logEvent("sensor_status", sensorStatus);
    updateFan();  // bei SENSOR_ERROR / offline sofort Fail-safe
  }
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.hostname("laptop-luefter");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WLAN verbinden");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  logEvent("wifi", "connected", WiFi.localIP().toString());
}

// Nicht blockierend: höchstens alle MQTT_RETRY_MS ein Verbindungsversuch.
void ensureMqtt() {
  if (mqtt.connected() || millis() - lastMqttAttempt < MQTT_RETRY_MS) {
    return;
  }
  lastMqttAttempt = millis();

  String clientId = String("laptop-luefter-") + String(ESP.getChipId(), HEX);
  const char* user = strlen(MQTT_USER) ? MQTT_USER : nullptr;
  const char* pass = strlen(MQTT_USER) ? MQTT_PASSWORD : nullptr;

  if (mqtt.connect(clientId.c_str(), user, pass, TOPIC_ONLINE, 1, true, "offline")) {
    mqtt.publish(TOPIC_ONLINE, "online", true);
    mqtt.subscribe(TOPIC_TEMP);
    mqtt.subscribe(TOPIC_SET);
    mqtt.subscribe(TOPIC_SENSOR_STATUS);
    publishState();
    logEvent("mqtt", "connected", MQTT_SERVER);
  } else {
    logEvent("mqtt", "failed", "rc=" + String(mqtt.state()));
  }
}

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
      setMode(mode == MODE_AUTO ? MODE_ON : MODE_AUTO, "taster");
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println();

  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(FAN_PIN, LOW);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  connectWifi();
  configTime(NTP_TZ, "pool.ntp.org");

  ArduinoOTA.setHostname("laptop-luefter");
  ArduinoOTA.begin();

  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(onMessage);
}

void loop() {
  ArduinoOTA.handle();
  ensureMqtt();
  mqtt.loop();
  handleButton();

  if (millis() - lastCheck >= CHECK_INTERVAL_MS) {
    lastCheck = millis();
    updateFan();
  }
}
