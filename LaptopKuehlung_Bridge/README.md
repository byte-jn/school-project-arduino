# LaptopKuehlung_Bridge

Verbindet die beiden Unos mit dem MQTT-Broker. Die Unos haben kein WLAN, die Brücke läuft deshalb auf dem Laptop.

```
Sensor-Uno (COM7) ─USB─► bridge.py ◄─MQTT─► Mosquitto ◄─MQTT─► bridge.py ─USB─► Lüfter-Uno (COM3)
```

## Voraussetzungen

- Python 3.11 und `python -m pip install --user -r requirements.txt`
- Docker Desktop

## Start

Im Ordner `LaptopKuehlung_Bridge`:

```bash
docker compose up -d
```

```bash
python bridge.py
```

Arduino IDE bzw. Serial Monitor vorher schließen, sonst sind die Ports belegt. Beenden mit Strg+C, Broker stoppen mit `docker compose down`.

| Option | Standard |
|---|---|
| `--sensor` | `COM7` |
| `--luefter` | `COM3` |
| `--broker` / `--port` | `127.0.0.1` / `1883` |
| `--debug` | rohe Rahmen auch auf der Konsole |

## Start-Diagnose

Die Brücke loggt beim Start WLAN (verbunden und sichtbare Netze), alle COM-Ports und ob der Broker erreichbar ist. Ist der Broker nach **2 Versuchen** nicht erreichbar, beendet sie sich mit Exit-Code 2. Ein belegter oder fehlender COM-Port wird alle 5 s neu versucht.

## MQTT-Topics

| Topic | Inhalt |
|---|---|
| `laptop/temperatur`, `laptop/luftfeuchte` | Messwerte (retained) |
| `laptop/sensor/status` | `ok`, `error`, `offline` |
| `laptop/luefter/set` | Befehl `ON`, `OFF`, `AUTO` |
| `laptop/luefter/status`, `/mode`, `/failsafe` | Zustand des Lüfters |
| `laptop/sensor/stats`, `laptop/luefter/stats` | Zusammenfassung, jede Minute |
| `laptop/bridge/status` | `online` / `offline` |
| `laptop/log` | jede Log-Zeile als JSON |

## Logs

Konsole und `logs/laptopkuehlung_<datum>.csv` mit den Spalten `datum;zeit_ms;geraet;level;ereignis;details`.

## Testen ohne Sensor

```bash
docker exec laptopkuehlung-mosquitto mosquitto_pub -t laptop/temperatur -m 27.5
```

```bash
docker exec laptopkuehlung-mosquitto mosquitto_sub -t "laptop/#" -v
```
