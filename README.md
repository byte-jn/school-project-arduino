# Arduino-Projekte

## Projekte

| Projekt | Board | Beschreibung |
|---|---|---|
| [Reaktionszeit](Reaktionszeit/Reaktionszeit.txt) | Arduino Uno (AVR) | LED leuchtet nach 1–3 s zufällig auf, Taster stoppt die Zeit, Ausgabe in ms im Serial Monitor. |
| [Temperatursensor](Temperatursensor/Temperatursensor.txt) | Arduino Uno (AVR) | DHT11 misst die Temperatur, Ampel-LEDs zeigen grün (bis 25 °C), gelb (über 25 °C) oder rot (über 26 °C). |
| [AnalogeDatenauslesung](AnalogeDatenauslesung/AnalogeDatenauslesung.txt) | Arduino Uno (AVR) | Potentiometer an A3 wird ausgelesen, drei LEDs (rot, gelb, blau) zeigen je ein Drittel des Wertebereichs, Rohwert im Serial Monitor. |
| [Wiederstandsmessen](Wiederstandsmessen/Wiederstandsmessen.txt) | Arduino Uno (AVR) | Platzhalter: schaltet bisher nur die eingebaute LED ein. |
| [LaptopKuehlung_Sensor](LaptopKuehlung_Sensor/LaptopKuehlung_Sensor.ino) | Arduino Uno (AVR) | Laptop-Kühlung, Seite 1: DHT22 misst alle 5 s, LED-Ampel zeigt den Zustand, Temperatur geht per Kabel an den Lüfter-Uno. |
| [LaptopKuehlung_Luefter](LaptopKuehlung_Luefter/LaptopKuehlung_Luefter.ino) | Arduino Uno (AVR) | Laptop-Kühlung, Seite 2: schaltet den Lüfter anhand der Temperatur (mit Hysterese und Fail-safe). |

## Dependencies

### Board-Core

| Core | Version | Für | Installation |
|---|---|---|---|
| Arduino AVR Boards | 1.8.8 | alle Projekte | Bei der Arduino IDE dabei |

In der IDE unter **Werkzeuge → Board** „Arduino Uno“ wählen und den COM-Port setzen.

### Bibliotheken (`libraries/`)

| Bibliothek | Version | Für | Hinweis |
|---|---|---|---|
| DHT sensor library (Adafruit) | 1.4.7 | Temperatursensor, LaptopKuehlung_Sensor | |
| Adafruit Unified Sensor | 1.1.15 | Temperatursensor, LaptopKuehlung_Sensor | Abhängigkeit der DHT sensor library |

Beide sind über **Werkzeuge → Bibliotheken verwalten** installierbar. Der Ordner `libraries/` ist per `.gitignore` vom Repository ausgeschlossen und muss auf einem neuen Rechner neu installiert werden.

`SoftwareSerial` (für die Kabelverbindung der Laptop-Kühlung) gehört zum Arduino AVR Core und muss nicht installiert werden. Reaktionszeit, AnalogeDatenauslesung und Wiederstandsmessen brauchen keine zusätzlichen Bibliotheken.

In `libraries/` sollen nur diese zwei Bibliotheken liegen. Andere Bibliotheken werden von keinem Sketch verwendet und können entfernt werden.

## Laptop-Kühlung (2 Arduino Unos, per Kabel verbunden)

Der Uno hat kein WLAN. Die zwei Seiten laufen deshalb auf zwei Unos, die über eine serielle Leitung verbunden sind: ein Kabel hin, ein Kabel zurück für die Bestätigung, plus GND.

```
LaptopKuehlung_Sensor (Uno + DHT22 + Ampel)        LaptopKuehlung_Luefter (Uno + Lüfter + Taster)
   D3  ── "temperatur;27.3" / "status;SENSOR_ERROR" ──►  D10      AN ab 26.0 °C, AUS unter 25.0 °C
   D4  ◄──────────── "ack;temperatur" ───────────────── D11
   GND ─────────────────────────────────────────────── GND
   │ USB (Log, 9600 Baud)                                │ USB (Log + Befehle ON/OFF/AUTO, 9600 Baud)
   └──────────────► Laptop: log-serial.ps1 ► CSV ◄──────┘
```

### Verdrahtung

**LaptopKuehlung_Sensor:**

| Bauteil | Anschluss |
|---|---|
| DHT22-Modul | `+ → 5V`, `OUT → D2`, `− → GND` (nackter 4-Pin-Sensor: 10 kΩ von DATA nach 5V) |
| LED rot / gelb / grün | `D13` / `D12` / `D11`, je ca. 220 Ω, kurzes Bein an GND |
| Kabel zum Lüfter-Uno | `D3 → D10` am Lüfter-Uno (Daten), `D4 ← D11` am Lüfter-Uno (Bestätigung), `GND → GND` am Lüfter-Uno |

Die LED-Pins und der DHT-Pin entsprechen dem alten [Temperatursensor](Temperatursensor/Temperatursensor.ino)-Aufbau.

| LED | Bedeutung |
|---|---|
| grün | Temperatur ≤ 25,0 °C |
| gelb | Temperatur > 25,0 °C |
| rot | Temperatur > 26,0 °C |
| rot blinkt | Sensorfehler (DHT22 liefert keinen Wert) |

**LaptopKuehlung_Luefter:**

```
5V ────────────┬────────── Lüfter +
               │
         1N4007 (Ring zu 5V)
               │
               ├────────── Lüfter −
               │
         PN2222 C
         PN2222 B ── 1 kΩ ── D9
         PN2222 E ────────── GND

D7  ── Taster ── GND
D10 ◄── D3 vom Sensor-Uno   (Daten)
D11 ──► D4 am Sensor-Uno    (Bestätigung)
GND ─── GND am Sensor-Uno
```

PN2222: flache Seite zu dir, Beine nach unten = E – B – C. Den Lüfter nie direkt an einen Pin hängen (max. ca. 20 mA).

### Verhalten des Lüfters

- **AUTO** (Standard): AN ab `TEMP_ON` (26,0 °C), AUS unter `TEMP_OFF` (25,0 °C). Dazwischen bleibt der Zustand, damit der Lüfter nicht ständig schaltet.
- **Fail-safe**, der Lüfter geht AN:
  - sofort, wenn der Sensor-Uno `status;SENSOR_ERROR` schickt,
  - wenn 60 s keine Temperatur kommt (Kabel ab, Sensor-Uno aus).
- **Taster an D7:** wechselt zwischen AUTO und dauerhaft AN.
- **Serial Monitor** des Lüfter-Unos (9600 Baud, Zeilenende „Neue Zeile“): `ON`, `OFF` oder `AUTO` eintippen.

Schwellen und Zeiten stehen oben im Sketch unter „Einstellungen“.

### Logging

Beide Unos schreiben CSV-Zeilen über USB in den Serial Monitor (9600 Baud). Die Zeit sind Sekunden seit dem Start, weil der Uno keine Uhr hat:

```
zeit_s;geraet;ereignis;wert;detail
5;sensor;send;temperatur=27.3;link=online
5;sensor;send_result;ok;18ms link=online
5;sensor;temp;27.3;hum=41%
5;luefter;fan;ON;temp>=26.0 temp=27.3
40;sensor;sensor_status;SENSOR_ERROR;DHT22 liefert NaN, Verkabelung pruefen
40;sensor;send;status=SENSOR_ERROR;link=online
40;sensor;send_result;ok;17ms link=online
40;luefter;failsafe;ON;sensor=SENSOR_ERROR
```

**Beim Start** schreiben beide Unos zuerst ihre Konfiguration (`info`-Zeilen: Board, Pins, Verbindung, Ziel bzw. Schwellen). Der Sensor-Uno testet danach einmal den Sensor und versucht bis zu **2-mal**, den Lüfter-Uno zu erreichen:

```
0;sensor;info;board;Arduino Uno, Sketch LaptopKuehlung_Sensor
0;sensor;info;verbindung;Kabel, SoftwareSerial 9600 Baud, Senden D3 -> Luefter D10, Empfangen D4 <- Luefter D11
0;sensor;info;ziel;LaptopKuehlung_Luefter, ack-Timeout 200 ms, 2 Verbindungsversuche
2;sensor;sensor_test;ok;23.4 C
2;sensor;connect;versuch 1/2;LaptopKuehlung_Luefter
2;sensor;send;status=online;link=unbekannt
2;sensor;send_result;timeout;200ms link=unsicher
3;sensor;connect;versuch 2/2;LaptopKuehlung_Luefter
3;sensor;send;status=online;link=unsicher
3;sensor;send_result;timeout;200ms link=offline
3;sensor;connect;abgebrochen;Luefter-Uno nach 2 Fehlschlaegen in Folge offline, es wird nicht mehr gesendet (Reset zum Neuversuch)
8;sensor;send;temperatur=23.4;link=offline
8;sensor;send_result;uebersprungen;link=offline, kein Versuch
```

| Verbindungsstatus | Bedeutung |
|---|---|
| `unbekannt` | direkt nach dem Start, noch nichts gesendet |
| `online` | letzte Nachricht wurde bestätigt |
| `unsicher` | letzter Versuch fehlgeschlagen, es wird weiter versucht |
| `offline` | 2 Fehlschläge in Folge (beim Start oder später): es wird **nicht mehr gesendet**, nur noch geloggt |

Ist die Verbindung offline, laufen Messung, Log und Ampel weiter, aber es gibt keinen Sendeversuch mehr. Erst ein Reset des Sensor-Unos versucht es neu. Der Lüfter-Uno geht ohne Daten nach 60 s in den Fail-safe (Lüfter AN).

Bei jedem Senden schreibt der Sensor-Uno zwei Zeilen:

| Zeile | Inhalt |
|---|---|
| `send` | was gesendet werden soll und der Verbindungsstatus **vor** dem Senden |
| `send_result` | Ergebnis: `ok` (Lüfter-Uno hat mit `ack` bestätigt), `abgelehnt` (`nak`, Nachricht ungültig), `timeout` (keine Antwort in 200 ms, z. B. Kabel ab oder Lüfter-Uno aus) oder `uebersprungen` (Verbindung offline, kein Versuch), dazu Dauer und Status **danach** |

Mit echter Uhrzeit in eine Datei mitschreiben: Serial Monitor schließen (der Port kann nur einmal offen sein), dann im Repository-Ordner, je Uno mit dessen COM-Port:

```bash
powershell -ExecutionPolicy Bypass -File .\log-serial.ps1 -Port COM7 -File luefter.csv
```

```bash
powershell -ExecutionPolicy Bypass -File .\log-serial.ps1 -Port COM8 -File sensor.csv
```

[log-serial.ps1](log-serial.ps1) setzt Datum und Uhrzeit vor jede Zeile und hängt sie an die Datei an. Beenden mit Strg+C. Die CSV-Dateien stehen in `.gitignore`.

### Test mit nur einem Uno

Den Lüfter-Uno kann man ohne Sensor-Uno testen: Ohne Daten geht er nach 60 s in den Fail-safe (Lüfter AN). Mit `OFF` im Serial Monitor schaltet er aus, mit `AUTO` wieder in den Automatikbetrieb.

Den Sensor-Uno kann man ohne Lüfter-Uno testen: Messung und Ampel laufen, die 2 Startversuche enden mit `timeout`, danach ist die Verbindung `offline` und jedes weitere Senden wird `uebersprungen`.
