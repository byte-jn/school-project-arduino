// Vorlage für die Zugangsdaten.
// Kopieren nach "secrets.h" (gleicher Ordner) und dort die echten Werte eintragen.
// secrets.h steht in .gitignore und wird nicht committet.
#pragma once

#define WIFI_SSID     "Dein_WLAN"
#define WIFI_PASSWORD "Dein_WLAN_Passwort"

#define MQTT_SERVER   "192.168.0.10"  // IP des Rechners, auf dem Mosquitto läuft
#define MQTT_PORT     1883
#define MQTT_USER     ""              // leer lassen, wenn der Broker keine Anmeldung verlangt
#define MQTT_PASSWORD ""
