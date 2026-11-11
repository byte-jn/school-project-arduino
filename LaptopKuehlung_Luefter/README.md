# LaptopKuehlung_Luefter

Schaltet einen 12-V-Lüfter über ein Relais. Die Temperatur kommt per USB von der [Brücke](../LaptopKuehlung_Bridge/), die sie über MQTT vom [Sensor](../LaptopKuehlung_Sensor/) bekommt.

## Hardware

| Bauteil | Anschluss |
|---|---|
| Arduino Uno | USB am Laptop (Standard: COM3) |
| Relais-Modul (High Level Trigger) | `IN → D3`, `VCC → 5V`, `GND → GND` |
| 12-V-Lüfter | über den Relais-Kontakt (`COM`/`NO`) an eigene 12-V-Versorgung |

Zieht das Relais bei LOW an, im Sketch `FAN_ACTIVE_LOW = true` setzen.

## Hochladen

Board **Arduino Uno**, Port **COM3**. Keine zusätzlichen Bibliotheken.

## Verhalten

| Situation | Lüfter |
|---|---|
| Temperatur ≥ 23,0 °C | AN |
| Temperatur < 22,0 °C | AUS |
| dazwischen | bleibt, wie er ist |
| Sensor meldet `error` oder `offline` | sofort AN (Fail-safe) |
| 60 s keine Temperatur | AN (Fail-safe) |

## Steuern

- **MQTT:** `laptop/luefter/set` mit `ON`, `OFF` oder `AUTO`
- **Ohne Brücke:** Serial Monitor (115200 Baud, Zeilenende „Neue Zeile“): `ON`, `OFF`, `AUTO` oder `STATUS`

Der Lüfter-Uno meldet jede Änderung (`fan`, `mode`, `failsafe`) an die Brücke. Die publiziert sie nach `laptop/luefter/...`.

## Log (USB, 115200 Baud)

```
6120;luefter;INFO;temp;nr=4,temp=27.3,hum=41
6121;luefter;INFO;fan;zustand=ON,grund=temp_hoch,temp=27.3,an_ab=23.0,aus_unter=22.0
```

Der Uno prüft Prüfsumme und laufende Nummer jedes Rahmens und meldet Fehler (`bad_frame`), Lücken (`nr_luecke`) und Duplikate.

## Einstellungen

Oben im Sketch: `FAN_PIN`, `FAN_ACTIVE_LOW`, `TEMP_ON`, `TEMP_OFF`, `TEMP_TIMEOUT_MS`, `ACK_TIMEOUT_MS`, `MAX_FAILS`, `LOG_FRAMES`.
