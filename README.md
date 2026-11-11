# Arduino-Projekte

## Projekte

| Projekt | Board | Beschreibung |
|---|---|---|
| [Reaktionszeit](Reaktionszeit/Reaktionszeit.txt) | Arduino Uno (AVR) | LED leuchtet nach 1–3 s zufällig auf, Taster stoppt die Zeit, Ausgabe in ms im Serial Monitor. |
| [Temperatursensor](Temperatursensor/Temperatursensor.txt) | Arduino Uno (AVR) | DHT11 misst die Temperatur, Ampel-LEDs zeigen grün (bis 25 °C), gelb (über 25 °C) oder rot (über 26 °C). |
| [AnalogeDatenauslesung](AnalogeDatenauslesung/AnalogeDatenauslesung.txt) | Arduino Uno (AVR) | Potentiometer an A3 wird ausgelesen, drei LEDs (rot, gelb, blau) zeigen je ein Drittel des Wertebereichs, Rohwert im Serial Monitor. |
| [Wiederstandsmessen](Wiederstandsmessen/Wiederstandsmessen.txt) | Arduino Uno (AVR) | Platzhalter: schaltet bisher nur die eingebaute LED ein. |
| [LaptopKuehlung_Sensor](LaptopKuehlung_Sensor/LaptopKuehlung_Sensor.ino) | Arduino Uno (AVR) | Laptop-Kühlung, Sensor: DHT22 misst alle 5 s, LED-Ampel, Daten per USB an die Brücke. |
| [LaptopKuehlung_Luefter](LaptopKuehlung_Luefter/LaptopKuehlung_Luefter.ino) | Arduino Uno (AVR) | Laptop-Kühlung, Lüfter: schaltet anhand der Temperatur (Hysterese, Fail-safe), bekommt die Daten per USB von der Brücke. |
| [LaptopKuehlung_Bridge](LaptopKuehlung_Bridge/bridge.py) | Laptop (Python + Docker) | Laptop-Kühlung, Brücke: verbindet beide Unos (USB) mit dem MQTT-Broker Mosquitto. |

## Dependencies

### Board-Core

| Core | Version | Für | Installation |
|---|---|---|---|
| Arduino AVR Boards | 1.8.8 | alle Sketches | Bei der Arduino IDE dabei |

In der IDE unter **Werkzeuge → Board** „Arduino Uno“ wählen und den COM-Port setzen.

### Bibliotheken (`libraries/`)

| Bibliothek | Version | Für | Hinweis |
|---|---|---|---|
| DHT sensor library (Adafruit) | 1.4.7 | Temperatursensor, LaptopKuehlung_Sensor | |
| Adafruit Unified Sensor | 1.1.15 | Temperatursensor, LaptopKuehlung_Sensor | Abhängigkeit der DHT sensor library |

Beide sind über **Werkzeuge → Bibliotheken verwalten** installierbar. Der Ordner `libraries/` ist per `.gitignore` vom Repository ausgeschlossen und muss auf einem neuen Rechner neu installiert werden. In `libraries/` sollen nur diese zwei Bibliotheken liegen.

### Laptop (für die Brücke)

| Was | Version | Installation |
|---|---|---|
| Python | 3.11 (getestet) | |
| paho-mqtt, pyserial | siehe [requirements.txt](LaptopKuehlung_Bridge/requirements.txt) | `python -m pip install --user -r LaptopKuehlung_Bridge/requirements.txt` |
| Docker Desktop | | für den Broker `eclipse-mosquitto:2` |

## Laptop-Kühlung

Der Uno hat kein WLAN. Beide Unos hängen deshalb per USB am Laptop. Die Brücke (`bridge.py`) liest beide USB-Ports und spricht mit dem MQTT-Broker. Die Unos sind nur über MQTT verbunden, nicht direkt.

```
Sensor-Uno (COM7) ──USB──┐                              ┌──USB── Lüfter-Uno (COM3)
  DHT22 + Ampel          ▼                              ▼          Relais + 12-V-Lüfter
                    bridge.py  ◄──── MQTT ────►  Mosquitto (Docker, 127.0.0.1:1883)
                         │
                         └──► Konsole + CSV (LaptopKuehlung_Bridge/logs/) + laptop/log
```

Ablauf einer Messung:

1. Der Sensor-Uno schickt `@D|12|temp=27.3&hum=41*..` per USB.
2. Die Brücke publiziert `laptop/temperatur` und bestätigt dem Sensor-Uno erst **danach** mit `@A|12|*..`. Kommt der Broker nicht nach, gibt es `@N|12|grund=mqtt_offline*..`.
3. Die Brücke bekommt `laptop/temperatur` vom Broker zurück und schickt `@T|..|temp=27.3&hum=41*..` an den Lüfter-Uno.
4. Der Lüfter-Uno bestätigt, regelt und meldet seinen Zustand (`@S|..|fan=ON&mode=AUTO&failsafe=0*..`). Die Brücke publiziert ihn nach `laptop/luefter/...`.

### Starten

1. Docker Desktop starten, dann den Broker:
   ```bash
   docker compose -f LaptopKuehlung_Bridge/docker-compose.yml up -d
   ```
2. `LaptopKuehlung_Sensor` auf den Uno an **COM7** laden, `LaptopKuehlung_Luefter` auf den Uno an **COM3**.
3. Arduino IDE schließen bzw. Serial Monitor zu (ein Port kann nur von einem Programm geöffnet werden).
4. Brücke starten (Standard: Sensor COM7, Lüfter COM3, Broker 127.0.0.1:1883):
   ```bash
   python LaptopKuehlung_Bridge/bridge.py
   ```
   Andere Ports: `--sensor COM8 --luefter COM5`. Rohe Rahmen auf der Konsole: `--debug`. Beenden mit Strg+C.

Beim Öffnen der Ports startet die Brücke beide Unos neu, sie melden sich dann mit ihren Boot-Infos.

### Start-Diagnose der Brücke

Die Brücke loggt beim Start:

| Ereignis | Inhalt |
|---|---|
| `bridge_start` | Version, Python, System, Log-Ordner |
| `wlan_verbunden` | Adapter, Status, SSID, Band, Kanal, Signal, Anmeldung |
| `wlan_verfuegbar` | jedes sichtbare WLAN mit Band, Signal, Anmeldung |
| `wlan_ziel` | Broker-Adresse und ob dafür WLAN nötig ist |
| `com_port`, `com_zuordnung` | alle COM-Ports (Beschreibung, USB-ID, Seriennummer) und ob die Ports für Sensor und Lüfter da sind |
| `mqtt_connect`, `broker_erreichbar` | Broker-Adresse, TCP-Test mit Antwortzeit |
| `abbruch` | Broker nach **2 Versuchen** nicht erreichbar: die Brücke beendet sich (Exit-Code 2) |

Ein belegter COM-Port (z. B. Serial Monitor offen) bricht nicht ab: Die Brücke meldet `port_unavailable` und versucht es alle 5 s erneut.

### Verdrahtung

**Sensor-Uno (COM7):**

| Bauteil | Anschluss |
|---|---|
| DHT22-Modul | `+ → 5V`, `OUT → D2`, `− → GND` (nackter 4-Pin-Sensor: 10 kΩ von DATA nach 5V) |
| LED rot / gelb / grün | `D13` / `D12` / `D11`, je ca. 220 Ω, kurzes Bein an GND |

| LED | Bedeutung |
|---|---|
| grün | Temperatur ≤ 25,0 °C |
| gelb | Temperatur > 25,0 °C |
| rot | Temperatur > 26,0 °C |
| rot blinkt | Sensorfehler (DHT22 liefert keinen Wert) |

**Lüfter-Uno (COM3):**

| Bauteil | Anschluss |
|---|---|
| Relais-Modul (High Level Trigger) | `IN → D3`, `VCC → 5V`, `GND → GND` |
| 12-V-Lüfter | über den Relais-Kontakt (`COM`/`NO`) an eine eigene 12-V-Versorgung |

Schaltet das Relais bei LOW („Low Level Trigger“), im Sketch `FAN_ACTIVE_LOW = true` setzen. Zwischen den beiden Unos gibt es **keine** Kabel.

### Nachrichten über USB

Auf jeder USB-Leitung (115200 Baud) laufen Log-Zeilen und Datenrahmen gemischt. Datenrahmen beginnen mit `@`:

```
@<TYP>|<NR>|<key=wert&key=wert>*<PRUEFSUMME>
```

| Teil | Bedeutung |
|---|---|
| `TYP` | `H` Hallo, `D` Daten, `S` Status/Zustand, `T` Temperatur, `C` Befehl, `A` Bestätigung, `N` Ablehnung |
| `NR` | laufende Nummer 1–65535 je Absender; `A`/`N` wiederholen die Nummer der beantworteten Nachricht |
| `PRUEFSUMME` | XOR aller Zeichen zwischen `@` und `*`, zwei Hex-Ziffern |

| Richtung | Rahmen |
|---|---|
| Sensor → Brücke | `H` (dev=sensor), `D` (temp, hum), `S` (sensor=ok/error) |
| Brücke → Sensor | `A`, `N` (grund=mqtt_offline, checksum, …) |
| Brücke → Lüfter | `H` (dev=bridge), `T` (temp, hum), `S` (sensor=ok/error/offline), `C` (mode=ON/OFF/AUTO) |
| Lüfter → Brücke | `H` (dev=luefter), `S` (fan, mode, failsafe), `A`, `N` |

Der Lüfter-Uno prüft die laufenden Nummern der Brücke und meldet Lücken (`nr_luecke`) und Duplikate.

### MQTT-Topics

| Topic | Von | Inhalt |
|---|---|---|
| `laptop/temperatur` | Brücke (Sensor) | Temperatur in °C, z. B. `27.3` (retained) |
| `laptop/luftfeuchte` | Brücke (Sensor) | Luftfeuchte in % (retained) |
| `laptop/sensor/status` | Brücke (Sensor) | `ok`, `error` oder `offline` (Sensor-Uno-Port weg) (retained) |
| `laptop/luefter/set` | du | `ON`, `OFF` oder `AUTO` |
| `laptop/luefter/status` | Brücke (Lüfter) | `ON` / `OFF` (retained) |
| `laptop/luefter/mode` | Brücke (Lüfter) | `AUTO` / `ON` / `OFF` (retained) |
| `laptop/luefter/failsafe` | Brücke (Lüfter) | `ON` / `OFF` (retained) |
| `laptop/sensor/stats`, `laptop/luefter/stats` | Brücke | Zusammenfassung als JSON, jede Minute |
| `laptop/bridge/status` | Brücke | `online` / `offline` (Last Will) |
| `laptop/log` | Brücke | jede Log-Zeile als JSON |

Alte (retained) Temperaturen gibt die Brücke beim Start nicht an den Lüfter weiter, sonst hielte er sie für aktuell.

### Verhalten des Lüfters

- **AUTO** (Standard): AN ab `TEMP_ON` (23,0 °C), AUS unter `TEMP_OFF` (22,0 °C). Dazwischen bleibt der Zustand.
- **Fail-safe**, der Lüfter geht AN:
  - sofort bei Sensorstatus `error` oder `offline`,
  - wenn 60 s keine Temperatur kommt (Brücke oder Sensor weg).
- **MQTT:** `laptop/luefter/set` mit `ON`, `OFF` oder `AUTO`. Ohne Brücke im Serial Monitor (115200 Baud) `ON`, `OFF`, `AUTO` oder `STATUS` eintippen.

Schwellen und Zeiten stehen oben im Sketch unter „Einstellungen“.

### Logging

Alle drei Teile loggen im gleichen Format:

```
zeit_ms;geraet;level;ereignis;key=wert,key=wert
```

`level` ist `DEBUG` (rohe Rahmen), `INFO`, `WARN` oder `ERROR`. Die Brücke setzt Datum und Uhrzeit davor und schreibt alles nach `LaptopKuehlung_Bridge/logs/laptopkuehlung_<datum>.csv` (Spalten `datum;zeit_ms;geraet;level;ereignis;details`). Beispiel:

```
2026-11-11 12:50:05;5012;sensor;INFO;send;typ=D,nr=3,link=online
2026-11-11 12:50:05;5013;sensor;INFO;measure;temp=27.3,hum=41
2026-11-11 12:50:05;31842;bridge;INFO;mqtt_publish;quelle=sensor,nr=3,inhalt=laptop/temperatur=27.3
2026-11-11 12:50:05;5030;sensor;INFO;send_ok;nr=3,rtt_ms=17,link=online
2026-11-11 12:50:05;6120;luefter;INFO;temp;nr=4,temp=27.3,hum=41
2026-11-11 12:50:05;6121;luefter;INFO;fan;zustand=ON,grund=temp_hoch,temp=27.3,an_ab=23.0,aus_unter=22.0
```

**Senden mit Status:** Sensor-Uno und Lüfter-Uno loggen bei jedem Senden den Verbindungsstatus vorher (`send`) und das Ergebnis nachher (`send_ok`, `send_nak`, `send_timeout`, `send_skipped`):

| Verbindungsstatus | Bedeutung |
|---|---|
| `unbekannt` | direkt nach dem Start |
| `online` | letzte Nachricht wurde bestätigt |
| `unsicher` | letzter Versuch fehlgeschlagen, es wird weiter versucht |
| `offline` | 2 Fehlschläge in Folge: es wird **nicht mehr gesendet** (`send_skipped`), bis der Uno neu startet. Startet die Brücke neu, setzt sie die Unos automatisch zurück. |

Jede Minute schreiben alle Unos eine Zusammenfassung (`stats`): gesendet, bestätigt, Timeouts, abgelehnt, übersprungen, Lücken, Duplikate, Lüfterzustand, letzte Temperatur.

### Test ohne Sensor

Nur Broker, Brücke und Lüfter-Uno laufen lassen und Werte selbst publizieren (im Broker-Container):

```bash
docker exec laptopkuehlung-mosquitto mosquitto_pub -t laptop/temperatur -m 27.5
```

```bash
docker exec laptopkuehlung-mosquitto mosquitto_pub -t laptop/luefter/set -m OFF
```

```bash
docker exec laptopkuehlung-mosquitto mosquitto_sub -t "laptop/#" -v
```
