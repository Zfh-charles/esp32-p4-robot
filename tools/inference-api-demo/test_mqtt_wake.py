#!/usr/bin/env python3
"""Publish a test MQTT wake to the device topic."""
import json
import os
import sys

import paho.mqtt.publish as mqtt_publish

device_id = sys.argv[1] if len(sys.argv) > 1 else "30:ed:a0:e1:b5:28"
broker = os.environ.get("MQTT_WAKE_BROKER", "broker.emqx.io")
port = int(os.environ.get("MQTT_WAKE_PORT", "1883"))
prefix = os.environ.get("MQTT_WAKE_TOPIC_PREFIX", "xiaozhi/reminder/wake")
topic = f"{prefix}/{device_id}"
payload = json.dumps({"type": "reminder_wake", "device_id": device_id})

mqtt_publish.single(topic, payload=payload, hostname=broker, port=port)
print(f"published to {topic} via {broker}:{port}")
