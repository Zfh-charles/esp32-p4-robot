#!/usr/bin/env python3
"""Wait for device MQTT subscribe, publish wake, capture serial response."""

import re
import ssl
import sys
import threading
import time

import paho.mqtt.client as mqtt
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM6"
BROKER = "114.245.179.11"
MQTT_PORT = 8883
TOPIC = "v1/notify/30eda0e1b528"
USER = "esp32_30eda0e1b528"
PWD = "829784e7eaa8e7d848f716dcbd84917a"

lines: list[str] = []
stop = False


def reader() -> None:
    ser = serial.Serial(PORT, 115200, timeout=0.3)
    while not stop:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", "replace").rstrip()
        lines.append(line)
        if any(k in line for k in ("ReminderMqtt", "ReminderPoll", "ReminderTrace", "Wake message")):
            print(line)
    ser.close()


def main() -> int:
    global stop
    t = threading.Thread(target=reader, daemon=True)
    t.start()
    print(f"Reading {PORT}, waiting for MQTT subscribed (max 90s)...")
    deadline = time.time() + 90
    subscribed = False
    while time.time() < deadline:
        if any("MQTT wake subscribed" in ln for ln in lines):
            subscribed = True
            break
        time.sleep(0.2)
    if not subscribed:
        print("FAIL: no 'MQTT wake subscribed' within 90s")
        stop = True
        t.join(timeout=2)
        return 1

    print(f"Device subscribed. Publishing to {TOPIC} ...")
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="probe-pc-wake")
    client.username_pw_set(USER, PWD)
    client.tls_set(cert_reqs=ssl.CERT_NONE)
    client.tls_insecure_set(True)
    client.connect(BROKER, MQTT_PORT, keepalive=30)
    client.publish(TOPIC, payload='{"type":"reminder_wake","probe":true}', qos=0).wait_for_publish(5)
    client.disconnect()

    print("Published. Waiting for device wake logs (15s)...")
    time.sleep(15)
    stop = True
    t.join(timeout=2)

    wake_msg = [ln for ln in lines if "Wake message" in ln]
    trigger = [ln for ln in lines if "MQTT wake -> trigger HTTP poll" in ln]
    poll_begin = [ln for ln in lines if "poll_begin" in ln]
    poll_skip = [ln for ln in lines if "poll_skip" in ln]

    print("\n=== RESULT ===")
    print(f"subscribed: {subscribed}")
    print(f"wake_message: {len(wake_msg)}")
    print(f"trigger_poll: {len(trigger)}")
    print(f"poll_begin_after_publish: {len([ln for ln in poll_begin if lines.index(ln) > max([lines.index(x) for x in lines if 'MQTT wake subscribed' in x], default=0)])}")

    if wake_msg:
        print("last_wake:", wake_msg[-1])
    if trigger:
        print("last_trigger:", trigger[-1])
    recent_skip = poll_skip[-3:] if poll_skip else []
    if recent_skip:
        print("recent_poll_skip:", *recent_skip, sep="\n  ")

    return 0 if wake_msg and trigger else 2


if __name__ == "__main__":
    sys.exit(main())
