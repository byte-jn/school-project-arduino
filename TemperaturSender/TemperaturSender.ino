/*
  TemperaturSender – Knoten 1 der Laptop-Kühlung

  Board:  NodeMCU 1.0 (ESP-12E Module), ESP8266-Core 3.1.2
  Libs:   DHT sensor library (+ Adafruit Unified Sensor), PubSubClient

  Verdrahtung DHT22-Modul:
    +   (VCC)  -> 3V3
    OUT (DATA) -> D5 (GPIO14)   (nackter 4-Pin-Sensor: zusätzlich 10 kΩ von DATA nach 3V3)
    -   (GND)  -> GND

  Verdrahtung LED-Ampel (jede LED mit ca. 220 Ω Vorwiderstand, kurzes Bein an GND):
    rot  -> D1 (GPIO5)
    gelb -> D2 (GPIO4)
    grün -> D6 (GPIO12)

  Anzeige:
    grün        Temperatur <= WARN_TEMP
    gelb        Temperatur >  WARN_TEMP
    rot         Temperatur >  BAD_TEMP
    rot blinkt  Sensorfehler

  Funktion:
    - misst alle 5 s die Temperatur und sendet sie nach  <prefix>temperatur  (retained, z. B. "27.3")
    - Status über  <prefix>sensor/status:  online / SENSOR_ERROR / offline (retained, Last Will)
    - schreibt Messungen und Statuswechsel als CSV-Zeile nach Serial und  <prefix>log

  Zugangsdaten: secrets.example.h nach secrets.h kopieren und ausfüllen.
*/

#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <time.h>
#include "secrets.h"

// ---------- Einstellungen ----------
// Muss in beiden Sketches gleich sein.
#define TOPIC_PREFIX "laptop/"

const char* DEVICE_NAME = "sensor";
const uint8_t DHT_PIN = D5;
const uint8_t DHT_TYPE = DHT22;
const uint8_t LED_RED = D1;
const uint8_t LED_YELLOW = D2;
const uint8_t LED_GREEN = D6;

const float WARN_TEMP = 25.0;   // darüber gelb
const float BAD_TEMP  = 26.0;   // darüber rot

const unsigned long MEASURE_INTERVAL_MS = 5000;   // DHT22 schafft höchstens alle 2 s eine Messung
const unsigned long MQTT_RETRY_MS = 5000;
const unsigned long BLINK_MS = 250;

const char* TOPIC_TEMP   = TOPIC_PREFIX "temperatur";
const char* TOPIC_STATUS = TOPIC_PREFIX "sensor/status";
const char* TOPIC_LOG    = TOPIC_PREFIX "log";

const char* NTP_TZ = "CET-1CEST,M3.5.0,M10.5.0/3";  // Mitteleuropa inkl. Sommerzeit
// -----------------------------------

DHT dht(DHT_PIN, DHT_TYPE);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

unsigned long lastMeasure = 0;
unsigned long lastMqttAttempt = 0;
bool sensorError = false;

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

const char* statusText() {
  return sensorError ? "SENSOR_ERROR" : "online";
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.hostname("laptop-sensor");
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
// So laufen Messung und LED-Ampel weiter, auch wenn der Broker weg ist.
void ensureMqtt() {
  if (mqtt.connected() || millis() - lastMqttAttempt < MQTT_RETRY_MS) {
    return;
  }
  lastMqttAttempt = millis();

  String clientId = String("laptop-sensor-") + String(ESP.getChipId(), HEX);
  const char* user = strlen(MQTT_USER) ? MQTT_USER : nullptr;
  const char* pass = strlen(MQTT_USER) ? MQTT_PASSWORD : nullptr;

  if (mqtt.connect(clientId.c_str(), user, pass, TOPIC_STATUS, 1, true, "offline")) {
    mqtt.publish(TOPIC_STATUS, statusText(), true);
    logEvent("mqtt", "connected", MQTT_SERVER);
  } else {
    logEvent("mqtt", "failed", "rc=" + String(mqtt.state()));
  }
}

void setSensorError(bool error) {
  if (error == sensorError) {
    return;
  }
  sensorError = error;
  mqtt.publish(TOPIC_STATUS, statusText(), true);
  logEvent("sensor_status", statusText(), error ? "DHT22 liefert NaN, Verkabelung prüfen" : "Sensor wieder ok");
}

void measure() {
  float temp = dht.readTemperature();
  float hum = dht.readHumidity();

  if (isnan(temp)) {
    // Kein Wert senden. Der Lüfter-Knoten geht bei SENSOR_ERROR sofort in den Fail-safe.
    setSensorError(true);
    return;
  }
  setSensorError(false);
  showTemperature(temp);

  char payload[8];
  dtostrf(temp, 1, 1, payload);
  mqtt.publish(TOPIC_TEMP, payload, true);

  String detail = isnan(hum) ? String("") : "hum=" + String(hum, 0) + "%";
  logEvent("temp", payload, detail);
}

void setup() {
  Serial.begin(115200);
  Serial.println();

  pinMode(LED_RED, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  setLeds(LOW, LOW, LOW);

  dht.begin();

  connectWifi();
  configTime(NTP_TZ, "pool.ntp.org");

  ArduinoOTA.setHostname("laptop-sensor");
  ArduinoOTA.begin();

  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
}

void loop() {
  ArduinoOTA.handle();
  ensureMqtt();
  mqtt.loop();

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
