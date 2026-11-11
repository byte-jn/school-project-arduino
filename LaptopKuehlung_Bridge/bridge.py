"""
Laptop-Brücke der Laptop-Kühlung.

Verbindet die beiden Arduino Unos (USB) mit dem MQTT-Broker:

    Sensor-Uno --USB--> bridge.py --MQTT--> Mosquitto --MQTT--> bridge.py --USB--> Lüfter-Uno

- Datenrahmen vom Sensor-Uno werden nach MQTT publiziert und erst bestätigt (A), wenn der
  Broker sie angenommen hat. Sonst gibt es eine Ablehnung (N).
- Temperatur, Sensorstatus und Befehle aus MQTT werden als Rahmen an den Lüfter-Uno geschickt.
- Zustandsmeldungen des Lüfter-Unos werden nach MQTT publiziert.
- Alle Log-Zeilen (Unos und Brücke) gehen auf die Konsole, in eine CSV-Datei und nach laptop/log.

Start:
    python bridge.py --sensor COM7 --luefter COM3

Beim Start prüft die Brücke WLAN, COM-Ports und Broker. Ist der Broker nach 2 Versuchen
nicht erreichbar, bricht sie ab (Exit-Code 2).
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import platform
import re
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

import paho.mqtt.client as mqtt
import serial
from serial.tools import list_ports

BRIDGE_VERSION = 3
BAUD = 115200
PREFIX = "laptop/"

T_TEMP = PREFIX + "temperatur"
T_HUM = PREFIX + "luftfeuchte"
T_SENSOR_STATUS = PREFIX + "sensor/status"
T_FAN_SET = PREFIX + "luefter/set"
T_FAN_STATUS = PREFIX + "luefter/status"
T_FAN_MODE = PREFIX + "luefter/mode"
T_FAN_FAILSAFE = PREFIX + "luefter/failsafe"
T_BRIDGE_STATUS = PREFIX + "bridge/status"
T_LOG = PREFIX + "log"

CONNECT_ATTEMPTS = 2           # Broker-Versuche beim Start, dann Abbruch
CONNECT_WAIT_S = 3.0           # so lange auf die Broker-Antwort warten
PUBLISH_TIMEOUT_S = 0.5        # so lange auf die Bestätigung des Brokers warten
ACK_TIMEOUT_S = 1.0            # so lange auf die Bestätigung des Lüfter-Unos warten
PORT_RETRY_S = 5.0             # COM-Port erneut öffnen nach Fehler
LEVELS = ("DEBUG", "INFO", "WARN", "ERROR")


# =====================================================================
# Logging (gleiches Format wie die Unos)
# =====================================================================
class Log:
    """Schreibt Log-Zeilen auf die Konsole, in die CSV-Datei und nach MQTT."""

    def __init__(self, logdir: Path, show_debug: bool):
        self.logdir = logdir
        self.show_debug = show_debug
        self.start = time.monotonic()
        self.lock = threading.Lock()
        self.mqtt: mqtt.Client | None = None
        self.logdir.mkdir(parents=True, exist_ok=True)

    def _csv_path(self) -> Path:
        return self.logdir / f"laptopkuehlung_{dt.date.today():%Y-%m-%d}.csv"

    def write(self, entry: dict) -> None:
        """entry: datum, zeit_ms, geraet, level, ereignis, details (dict)."""
        details = ",".join(f"{k}={v}" for k, v in entry["details"].items())
        line = f"{entry['datum']};{entry['zeit_ms']};{entry['geraet']};{entry['level']};{entry['ereignis']};{details}"
        with self.lock:
            if entry["level"] != "DEBUG" or self.show_debug:
                print(line, flush=True)
            path = self._csv_path()
            new = not path.exists()
            with path.open("a", encoding="utf-8", newline="") as f:
                if new:
                    f.write("datum;zeit_ms;geraet;level;ereignis;details\n")
                f.write(line + "\n")
        client = self.mqtt
        if client is not None and client.is_connected():
            client.publish(T_LOG, json.dumps(entry, ensure_ascii=False), qos=0)

    def bridge(self, level: str, event: str, **details) -> None:
        self.write({
            "datum": now_str(),
            "zeit_ms": int((time.monotonic() - self.start) * 1000),
            "geraet": "bridge",
            "level": level,
            "ereignis": event,
            "details": {k: v for k, v in details.items() if v is not None},
        })


def now_str() -> str:
    return dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def parse_log_line(line: str) -> dict | None:
    """Zerlegt 'zeit_ms;geraet;LEVEL;ereignis;k=v,k=v'. Gibt None zurück, wenn es keine Log-Zeile ist."""
    parts = line.split(";", 4)
    if len(parts) != 5 or not parts[0].isdigit() or parts[2] not in LEVELS:
        return None
    details: dict[str, str] = {}
    last_key = None
    for item in parts[4].split(","):
        if "=" in item:
            key, value = item.split("=", 1)
            details[key] = value
            last_key = key
        elif last_key is not None:
            details[last_key] += "," + item   # Komma im Text, z. B. bei "hinweis"
    return {
        "datum": now_str(),
        "zeit_ms": int(parts[0]),
        "geraet": parts[1],
        "level": parts[2],
        "ereignis": parts[3],
        "details": details,
    }


# =====================================================================
# Datenrahmen: @<TYP>|<NR>|<key=wert&key=wert>*<PRUEFSUMME>
# =====================================================================
def checksum(body: str) -> int:
    cs = 0
    for ch in body.encode("ascii", errors="replace"):
        cs ^= ch
    return cs


def build_frame(ftype: str, seq: int, payload: dict | None = None) -> str:
    text = "&".join(f"{k}={v}" for k, v in (payload or {}).items())
    body = f"{ftype}|{seq}|{text}"
    return f"@{body}*{checksum(body):02X}"


def parse_frame(body: str) -> tuple[str | None, int, dict, str | None]:
    """body ohne '@'. Gibt (typ, nr, payload, fehler) zurück; typ ist None bei kaputtem Rahmen."""
    star = body.rfind("*")
    if star < 0 or len(body) - star != 3:
        return None, 0, {}, "format"
    try:
        expected = int(body[star + 1:], 16)
    except ValueError:
        return None, 0, {}, "format"
    content = body[:star]
    if checksum(content) != expected:
        return None, 0, {}, "checksum"
    fields = content.split("|", 2)
    if len(fields) != 3 or len(fields[0]) != 1 or not fields[1].isdigit():
        return None, 0, {}, "format"
    payload = {}
    for pair in fields[2].split("&"):
        if "=" in pair:
            k, v = pair.split("=", 1)
            payload[k] = v
    return fields[0], int(fields[1]), payload, None


# =====================================================================
# Verbindung zu einem Uno
# =====================================================================
class UnoPort(threading.Thread):
    def __init__(self, role: str, port: str, bridge: "Bridge"):
        super().__init__(daemon=True, name=f"uno-{role}")
        self.role = role
        self.port = port
        self.bridge = bridge
        self.log = bridge.log
        self.ser: serial.Serial | None = None
        self.write_lock = threading.Lock()
        self.seq_lock = threading.Lock()
        self.next_seq = 1
        self.last_open_error: str | None = None
        self.pending: dict[int, tuple[float, str]] = {}
        self.pending_lock = threading.Lock()
        self.online = False
        self.stop_event = threading.Event()

    # ---------- Senden ----------
    def send_frame(self, ftype: str, payload: dict | None = None, expect_ack: bool = True) -> int | None:
        if self.ser is None or not self.ser.is_open:
            self.log.bridge("WARN", "send_skipped", ziel=self.role, typ=ftype, grund="port_geschlossen")
            return None
        # send_frame wird aus dem MQTT-Thread und dem USB-Thread aufgerufen.
        with self.seq_lock:
            seq = self.next_seq
            self.next_seq = self.next_seq + 1 if self.next_seq < 65535 else 1
        frame = build_frame(ftype, seq, payload)
        if expect_ack:
            with self.pending_lock:
                self.pending[seq] = (time.monotonic(), ftype)
        self.log.bridge("INFO", "send", ziel=self.role, typ=ftype, nr=seq,
                        link="online" if self.online else "unbekannt")
        self._write(frame)
        return seq

    def reply(self, ok: bool, seq: int, reason: str = "") -> None:
        frame = build_frame("A", seq) if ok else build_frame("N", seq, {"grund": reason})
        self._write(frame)

    def _write(self, frame: str) -> None:
        try:
            with self.write_lock:
                self.ser.write((frame + "\n").encode("ascii"))
            self.log.bridge("DEBUG", "tx", ziel=self.role, frame=frame[1:])
        except (serial.SerialException, AttributeError) as exc:
            self.log.bridge("ERROR", "write_failed", ziel=self.role, fehler=str(exc))

    def check_timeouts(self) -> None:
        now = time.monotonic()
        with self.pending_lock:
            expired = [(s, t) for s, t in self.pending.items() if now - t[0] > ACK_TIMEOUT_S]
            for seq, _ in expired:
                del self.pending[seq]
        for seq, (_, ftype) in expired:
            self.online = False
            self.log.bridge("WARN", "send_timeout", ziel=self.role, typ=ftype, nr=seq)

    def _ack_received(self, ok: bool, seq: int, payload: dict) -> None:
        with self.pending_lock:
            entry = self.pending.pop(seq, None)
        if entry is None:
            return
        rtt = int((time.monotonic() - entry[0]) * 1000)
        if ok:
            self.online = True
            self.log.bridge("INFO", "send_ok", ziel=self.role, typ=entry[1], nr=seq, rtt_ms=rtt)
        else:
            self.log.bridge("WARN", "send_nak", ziel=self.role, typ=entry[1], nr=seq, rtt_ms=rtt,
                            antwort=payload.get("grund", "?"))

    # ---------- Empfangen ----------
    def run(self) -> None:
        while not self.stop_event.is_set():
            if not self._open():
                self.stop_event.wait(PORT_RETRY_S)
                continue
            try:
                self._read_loop()
            except serial.SerialException as exc:
                self.log.bridge("ERROR", "port_lost", rolle=self.role, port=self.port, fehler=str(exc))
            self.online = False
            self.bridge.port_closed(self.role)
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None
            self.stop_event.wait(PORT_RETRY_S)

    def _open(self) -> bool:
        try:
            # Öffnen setzt den Uno zurück (DTR). Er startet neu und schickt seine Boot-Infos.
            self.ser = serial.Serial(self.port, BAUD, timeout=0.2)
            self.last_open_error = None
            self.log.bridge("INFO", "port_open", rolle=self.role, port=self.port, baud=BAUD,
                            hinweis="Uno startet neu")
            return True
        except serial.SerialException as exc:
            text = str(exc)
            busy = "Zugriff" in text or "Access" in text
            hint = "Serial Monitor der Arduino IDE schliessen" if busy else "USB-Kabel und Port pruefen"
            # Nur beim ersten Mal bzw. wenn sich der Grund ändert loggen, nicht alle 5 s.
            if hint != self.last_open_error:
                self.last_open_error = hint
                self.log.bridge("WARN", "port_unavailable", rolle=self.role, port=self.port,
                                hinweis=hint, neuer_versuch_alle_s=int(PORT_RETRY_S))
            return False

    def _read_loop(self) -> None:
        buf = b""
        while not self.stop_event.is_set():
            chunk = self.ser.read(256)
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", errors="replace").strip()
                if line:
                    self._handle_line(line)

    def _handle_line(self, line: str) -> None:
        if line.startswith("@"):
            self._handle_frame(line[1:])
            return
        entry = parse_log_line(line)
        if entry is None:
            if not line.startswith("zeit_ms;"):
                self.log.bridge("DEBUG", "uno_text", rolle=self.role, text=line)
            return
        if entry["geraet"] != self.role:
            self.log.bridge("ERROR", "falscher_uno", port=self.port, erwartet=self.role,
                            gemeldet=entry["geraet"], hinweis="--sensor/--luefter vertauscht?")
        self.log.write(entry)
        if entry["ereignis"] == "stats":
            self.bridge.publish(f"{PREFIX}{entry['geraet']}/stats", json.dumps(entry["details"]), retain=True)

    def _handle_frame(self, body: str) -> None:
        self.log.bridge("DEBUG", "rx", quelle=self.role, frame=body)
        ftype, seq, payload, error = parse_frame(body)
        if ftype is None:
            self.log.bridge("ERROR", "bad_frame", quelle=self.role, grund=error, frame=body)
            self.reply(False, 0, error or "format")
            return
        if ftype in ("A", "N"):
            self._ack_received(ftype == "A", seq, payload)
            return
        if ftype == "H":
            dev = payload.get("dev", "?")
            self.online = True
            self.log.bridge("INFO", "uno_hallo", port=self.port, rolle=self.role, dev=dev,
                            protokoll=payload.get("ver", "?"))
            if dev != self.role:
                self.log.bridge("ERROR", "falscher_uno", port=self.port, erwartet=self.role, gemeldet=dev)
            if seq != 0:
                self.reply(True, seq)
            self.bridge.uno_hello(self.role)
            return
        self.bridge.handle_uno_frame(self, ftype, seq, payload)


# =====================================================================
# Brücke
# =====================================================================
class Bridge:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.log = Log(Path(args.logdir), args.debug)
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="laptopkuehlung-bridge")
        self.client.will_set(T_BRIDGE_STATUS, "offline", qos=1, retain=True)
        self.client.reconnect_delay_set(min_delay=1, max_delay=10)
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message = self._on_message
        self.connected = threading.Event()
        self.last_hum: str | None = None
        self.sensor_status: str | None = None
        self.unos: dict[str, UnoPort] = {}
        if args.sensor:
            self.unos["sensor"] = UnoPort("sensor", args.sensor, self)
        if args.luefter:
            self.unos["luefter"] = UnoPort("luefter", args.luefter, self)

    # ---------- MQTT ----------
    def _on_connect(self, client, userdata, flags, reason_code, properties) -> None:
        if reason_code.is_failure:
            self.log.bridge("ERROR", "mqtt_connect_failed", grund=str(reason_code))
            return
        self.connected.set()
        self.log.mqtt = client
        client.publish(T_BRIDGE_STATUS, "online", qos=1, retain=True)
        for topic in (T_TEMP, T_HUM, T_SENSOR_STATUS, T_FAN_SET):
            client.subscribe(topic, qos=1)
        self.log.bridge("INFO", "mqtt_connected", broker=f"{self.args.broker}:{self.args.port}",
                        abonniert=f"{T_TEMP}|{T_HUM}|{T_SENSOR_STATUS}|{T_FAN_SET}")

    def _on_disconnect(self, client, userdata, flags, reason_code, properties) -> None:
        self.connected.clear()
        self.log.bridge("WARN", "mqtt_disconnected", grund=str(reason_code), hinweis="automatischer Neuversuch")

    def _on_message(self, client, userdata, msg) -> None:
        payload = msg.payload.decode("utf-8", errors="replace").strip()
        fan = self.unos.get("luefter")
        if msg.topic == T_HUM:
            self.last_hum = payload
            return
        if msg.topic == T_SENSOR_STATUS:
            self.sensor_status = payload   # auch Werte vom Broker merken (z. B. retained nach Neustart)
        if fan is None:
            return
        if msg.topic == T_TEMP:
            if msg.retain:
                # Alter Wert vom Broker: nicht weitergeben, sonst hielte der Lüfter ihn für aktuell.
                self.log.bridge("DEBUG", "retained_ignoriert", topic=msg.topic, wert=payload)
                return
            data = {"temp": payload}
            if self.last_hum:
                data["hum"] = self.last_hum
            fan.send_frame("T", data)
        elif msg.topic == T_SENSOR_STATUS:
            fan.send_frame("S", {"sensor": payload})
        elif msg.topic == T_FAN_SET:
            mode = payload.upper()
            if mode not in ("ON", "OFF", "AUTO"):
                self.log.bridge("WARN", "invalid_command", topic=msg.topic, wert=payload)
                return
            self.log.bridge("INFO", "befehl", topic=msg.topic, modus=mode)
            fan.send_frame("C", {"mode": mode})

    def publish(self, topic: str, payload: str, retain: bool = False) -> bool:
        """Publiziert mit QoS 1 und wartet auf die Bestätigung des Brokers."""
        if not self.client.is_connected():
            return False
        try:
            info = self.client.publish(topic, payload, qos=1, retain=retain)
            if info.rc != mqtt.MQTT_ERR_SUCCESS:
                return False
            info.wait_for_publish(timeout=PUBLISH_TIMEOUT_S)
            return info.is_published()
        except (RuntimeError, ValueError):
            return False

    # ---------- Rahmen von den Unos ----------
    def handle_uno_frame(self, uno: UnoPort, ftype: str, seq: int, payload: dict) -> None:
        if uno.role == "sensor" and ftype == "D":
            temp = payload.get("temp")
            if temp is None:
                uno.reply(False, seq, "temp_fehlt")
                self.log.bridge("ERROR", "bad_frame", quelle="sensor", grund="temp_fehlt", nr=seq)
                return
            ok = True
            if "hum" in payload:
                ok = self.publish(T_HUM, payload["hum"], retain=True) and ok
            ok = self.publish(T_TEMP, temp, retain=True) and ok
            if self.sensor_status != "ok":
                ok = self.publish(T_SENSOR_STATUS, "ok", retain=True) and ok
                if ok:
                    self.sensor_status = "ok"
            self._answer_publish(uno, seq, ok, f"{T_TEMP}={temp}")
        elif uno.role == "sensor" and ftype == "S":
            status = payload.get("sensor", "?")
            ok = self.publish(T_SENSOR_STATUS, status, retain=True)
            if ok:
                self.sensor_status = status
            self._answer_publish(uno, seq, ok, f"{T_SENSOR_STATUS}={status}")
        elif uno.role == "luefter" and ftype == "S":
            ok = True
            for key, topic in (("fan", T_FAN_STATUS), ("mode", T_FAN_MODE), ("failsafe", T_FAN_FAILSAFE)):
                if key in payload:
                    value = payload[key]
                    if key == "failsafe":
                        value = "ON" if value == "1" else "OFF"
                    ok = self.publish(topic, value, retain=True) and ok
            self._answer_publish(uno, seq, ok, "luefter_zustand=" + "&".join(f"{k}={v}" for k, v in payload.items()))
        else:
            uno.reply(False, seq, "typ_unbekannt")
            self.log.bridge("ERROR", "bad_frame", quelle=uno.role, grund="typ_unbekannt", typ=ftype, nr=seq)

    def _answer_publish(self, uno: UnoPort, seq: int, ok: bool, what: str) -> None:
        if ok:
            uno.reply(True, seq)
            self.log.bridge("INFO", "mqtt_publish", quelle=uno.role, nr=seq, inhalt=what)
        else:
            uno.reply(False, seq, "mqtt_offline")
            self.log.bridge("WARN", "mqtt_publish_failed", quelle=uno.role, nr=seq, inhalt=what,
                            hinweis="Uno bekommt Ablehnung (N)")

    def uno_hello(self, role: str) -> None:
        if role == "luefter":
            # Brücke meldet sich, der Lüfter-Uno setzt damit seine Nummernprüfung zurück.
            fan = self.unos["luefter"]
            fan.send_frame("H", {"dev": "bridge", "ver": BRIDGE_VERSION})
            if self.sensor_status:
                fan.send_frame("S", {"sensor": self.sensor_status})

    def port_closed(self, role: str) -> None:
        if role == "sensor":
            # Sensor-Uno weg: Lüfter soll sofort in den Fail-safe.
            if self.publish(T_SENSOR_STATUS, "offline", retain=True):
                self.sensor_status = "offline"
            self.log.bridge("WARN", "sensor_offline", hinweis="Luefter geht in den Fail-safe")

    # ---------- Start-Diagnose ----------
    def diagnose(self) -> None:
        self.log.bridge("INFO", "bridge_start", version=BRIDGE_VERSION, python=platform.python_version(),
                        system=f"{platform.system()} {platform.release()}", log_ordner=str(self.log.logdir.resolve()))
        self._diagnose_wlan()
        self._diagnose_ports()

    def _diagnose_wlan(self) -> None:
        if platform.system() != "Windows":
            self.log.bridge("INFO", "wlan", hinweis="WLAN-Diagnose nur unter Windows")
            return
        iface = netsh_pairs(["netsh", "wlan", "show", "interfaces"])
        if not iface:
            self.log.bridge("WARN", "wlan", status="kein WLAN-Adapter gefunden")
        else:
            channel = first(iface, "Kanal", "Channel")
            self.log.bridge(
                "INFO", "wlan_verbunden",
                adapter=first(iface, "Name"),
                status=first(iface, "Status", "State", "Zustand"),
                ssid=first(iface, "SSID"),
                band=band_of(channel, first(iface, "Band")),
                kanal=channel,
                signal=first(iface, "Signal"),
                auth=first(iface, "Authentifizierung", "Authentication"),
            )
        for net in netsh_networks():
            self.log.bridge("INFO", "wlan_verfuegbar", **net)
        local = self.args.broker in ("127.0.0.1", "localhost", "::1")
        self.log.bridge("INFO", "wlan_ziel",
                        broker=f"{self.args.broker}:{self.args.port}",
                        hinweis="Broker laeuft lokal, WLAN fuer die Bruecke nicht noetig" if local
                        else "Broker im Netz, WLAN-Verbindung noetig")

    def _diagnose_ports(self) -> None:
        found = {}
        for p in list_ports.comports():
            found[p.device] = p
            vidpid = f"{p.vid:04X}:{p.pid:04X}" if p.vid is not None else "-"
            self.log.bridge("INFO", "com_port", port=p.device, beschreibung=p.description,
                            usb=vidpid, seriennr=p.serial_number or "-")
        if not found:
            self.log.bridge("WARN", "com_port", status="keine COM-Ports gefunden")
        for role, uno in self.unos.items():
            if uno.port in found:
                self.log.bridge("INFO", "com_zuordnung", rolle=role, port=uno.port, status="vorhanden")
            else:
                self.log.bridge("WARN", "com_zuordnung", rolle=role, port=uno.port, status="fehlt",
                                hinweis="USB-Kabel pruefen, wird alle 5 s erneut versucht")

    def connect_broker(self) -> bool:
        target = f"{self.args.broker}:{self.args.port}"
        for attempt in range(1, CONNECT_ATTEMPTS + 1):
            self.log.bridge("INFO", "mqtt_connect", broker=target, versuch=attempt, von=CONNECT_ATTEMPTS)
            reachable, detail = tcp_check(self.args.broker, self.args.port)
            self.log.bridge("INFO" if reachable else "WARN", "broker_erreichbar",
                            broker=target, tcp="ja" if reachable else "nein", detail=detail)
            if reachable:
                try:
                    self.client.connect(self.args.broker, self.args.port, keepalive=30)
                    self.client.loop_start()
                    if self.connected.wait(CONNECT_WAIT_S):
                        return True
                    self.client.loop_stop()
                    self.log.bridge("WARN", "mqtt_connect_timeout", broker=target, warte_s=CONNECT_WAIT_S)
                except OSError as exc:
                    self.log.bridge("WARN", "mqtt_connect_failed", broker=target, fehler=str(exc))
            if attempt < CONNECT_ATTEMPTS:
                time.sleep(1)
        self.log.bridge("ERROR", "abbruch", grund="broker_nicht_erreichbar", broker=target,
                        versuche=CONNECT_ATTEMPTS,
                        hinweis="Broker starten: docker compose up -d (im Ordner LaptopKuehlung_Bridge)")
        return False

    # ---------- Hauptschleife ----------
    def run(self) -> int:
        self.diagnose()
        if not self.connect_broker():
            return 2
        if not self.unos:
            self.log.bridge("WARN", "keine_unos", hinweis="--sensor und/oder --luefter angeben")
        for uno in self.unos.values():
            uno.start()
        try:
            while True:
                for uno in self.unos.values():
                    uno.check_timeouts()
                time.sleep(0.2)
        except KeyboardInterrupt:
            self.log.bridge("INFO", "bridge_stop", grund="Strg+C")
        finally:
            for uno in self.unos.values():
                uno.stop_event.set()
            if self.client.is_connected():
                self.client.publish(T_BRIDGE_STATUS, "offline", qos=1, retain=True).wait_for_publish(1)
            self.client.disconnect()
            self.client.loop_stop()
        return 0


# =====================================================================
# Hilfsfunktionen
# =====================================================================
def tcp_check(host: str, port: int) -> tuple[bool, str]:
    start = time.monotonic()
    try:
        with socket.create_connection((host, port), timeout=2):
            return True, f"{int((time.monotonic() - start) * 1000)} ms"
    except OSError as exc:
        return False, exc.strerror or str(exc)


def run_netsh(cmd: list[str]) -> str:
    try:
        out = subprocess.run(cmd, capture_output=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        return ""
    for enc in ("utf-8", "cp850", "cp1252"):
        try:
            return out.stdout.decode(enc)
        except UnicodeDecodeError:
            continue
    return out.stdout.decode("utf-8", errors="replace")


def netsh_pairs(cmd: list[str]) -> dict[str, str]:
    pairs: dict[str, str] = {}
    for line in run_netsh(cmd).splitlines():
        if ":" in line:
            key, value = line.split(":", 1)
            key = key.strip()
            if key and key not in pairs:
                pairs[key] = value.strip()
    return pairs


def netsh_networks() -> list[dict]:
    networks: list[dict] = []
    current: dict | None = None
    for line in run_netsh(["netsh", "wlan", "show", "networks", "mode=bssid"]).splitlines():
        m = re.match(r"^SSID \d+\s*:\s*(.*)$", line.strip())
        if m:
            current = {"ssid": m.group(1) or "(versteckt)"}
            networks.append(current)
            continue
        if current is None or ":" not in line:
            continue
        key, value = (s.strip() for s in line.split(":", 1))
        if key in ("Authentifizierung", "Authentication") and "auth" not in current:
            current["auth"] = value
        elif key == "Signal" and "signal" not in current:
            current["signal"] = value
        elif key in ("Kanal", "Channel") and "kanal" not in current:
            current["kanal"] = value
            current["band"] = band_of(value, None)
        elif key == "Band" and value:
            current["band"] = value
    return networks


def first(pairs: dict, *keys: str) -> str | None:
    for key in keys:
        if key in pairs and pairs[key]:
            return pairs[key]
    return None


def band_of(channel: str | None, band: str | None) -> str | None:
    if band:
        return band
    if channel and channel.isdigit():
        return "2,4 GHz" if int(channel) <= 14 else "5 GHz"
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description="Laptop-Brücke der Laptop-Kühlung (USB <-> MQTT)")
    parser.add_argument("--sensor", default="COM7", help="COM-Port des Sensor-Unos (Standard: COM7)")
    parser.add_argument("--luefter", default="COM3", help="COM-Port des Lüfter-Unos (Standard: COM3)")
    parser.add_argument("--broker", default="127.0.0.1", help="MQTT-Broker (Standard: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=1883, help="MQTT-Port (Standard: 1883)")
    parser.add_argument("--logdir", default=str(Path(__file__).with_name("logs")), help="Ordner für die CSV-Logs")
    parser.add_argument("--debug", action="store_true", help="auch DEBUG-Zeilen (rohe Rahmen) auf der Konsole")
    args = parser.parse_args()
    if args.sensor.lower() == "none":
        args.sensor = None
    if args.luefter.lower() == "none":
        args.luefter = None
    return Bridge(args).run()


if __name__ == "__main__":
    sys.exit(main())
