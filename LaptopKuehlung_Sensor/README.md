# LaptopKuehlung_Sensor

Misst alle 5 s Temperatur und Luftfeuchte und schickt sie per USB an die [Brücke](../LaptopKuehlung_Bridge/). Eine LED-Ampel zeigt die Temperatur direkt am Board.

## Hardware

| Bauteil | Anschluss |
|---|---|
| Arduino Uno | USB am Laptop (Standard: COM7) |
| DHT22-Modul | `+ → 5V`, `OUT → D2`, `− → GND` |
| LED rot / gelb / grün | `D13` / `D12` / `D11`, je ca. 220 Ω, kurzes Bein an GND |

## Hochladen

Board **Arduino Uno**, Port **COM7**. Bibliotheken: *DHT sensor library* und *Adafruit Unified Sensor*.

## Verhalten

| LED | Bedeutung |
|---|---|
| grün | ≤ 25,0 °C |
| gelb | > 25,0 °C |
| rot | > 26,0 °C |
| rot blinkt | Sensorfehler |

- Jede Messung geht als Rahmen `@D|<nr>|temp=27.3&hum=41*<prüfsumme>` an die Brücke.
- Die Brücke bestätigt erst, wenn der MQTT-Broker die Nachricht hat.
- Nach **2 Fehlschlägen in Folge** gilt die Verbindung als `offline`: Es wird nicht mehr gesendet, bis der Uno neu startet. Messung und Ampel laufen weiter.

## Log (USB, 115200 Baud)

```
5012;sensor;INFO;send;typ=D,nr=3,link=online
5030;sensor;INFO;send_ok;nr=3,rtt_ms=17,link=online
5031;sensor;INFO;measure;temp=27.3,hum=41
```

Format: `zeit_ms;geraet;level;ereignis;details`. Jede Minute kommt eine Zusammenfassung (`stats`).

## Einstellungen

Oben im Sketch: Pins, Ampel-Schwellen (`WARN_TEMP`, `BAD_TEMP`), Messintervall, `ACK_TIMEOUT_MS`, `MAX_FAILS`, `LOG_FRAMES`.
