#!/usr/bin/env python3
"""Check what a Fancy2WLED board publishes to your MQTT broker (for Home Assistant discovery).

    pip install paho-mqtt jinja2          (jinja2 is optional: it lets the value templates be rendered like Home Assistant does)
    python tools/check_mqtt.py <broker> [--port 1883] [--user U --password P] [--discovery homeassistant]

It listens for a few seconds, then checks that every discovery message has what Home Assistant needs, that unique ids are unique,
that the topics hang off the board's own prefix, and that each value template renders to something valid for the live state.
"""
import argparse
import json
import sys
import time

import paho.mqtt.client as mqtt


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("broker")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--user")
    ap.add_argument("--password")
    ap.add_argument("--discovery", default="homeassistant")
    ap.add_argument("--seconds", type=float, default=4.0)
    a = ap.parse_args()

    msgs = []
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="fancy2wled-check")
    if a.user:
        c.username_pw_set(a.user, a.password)
    latest = {}                      # MQTT may deliver a message twice (QoS 1 is at-least-once): keep the last one per topic
    c.on_message = lambda cl, ud, m: latest.__setitem__(m.topic, (m.topic, m.payload.decode("utf-8", "replace"), bool(m.retain)))
    c.connect(a.broker, a.port)
    c.subscribe("#", 1)
    c.loop_start()
    time.sleep(a.seconds)
    c.loop_stop()
    c.disconnect()
    msgs = list(latest.values())

    disc = [(t, json.loads(p)) for t, p, _ in msgs if t.startswith(a.discovery + "/") and t.endswith("/config") and "fancy2wled" in p and p.strip()]
    if not disc:
        print("no Fancy2WLED discovery messages found. Is the board connected (setup page -> Home Assistant (MQTT))?")
        return 1
    base = disc[0][1]["availability_topic"].rsplit("/", 1)[0]
    states = [p for t, p, _ in msgs if t == base + "/state"]
    status = [p for t, p, _ in msgs if t == base + "/status"]
    state = json.loads(states[-1]) if states else None
    print("board prefix: %s   status: %s   entities announced: %d" % (base, status[-1] if status else "?", len(disc)))
    try:
        from jinja2 import Environment
        env = Environment()
    except ImportError:
        env = None
        print("(jinja2 not installed: templates are not rendered)")

    problems, ids = [], set()
    need = {"switch": ["command_topic", "state_topic"], "sensor": ["state_topic"], "binary_sensor": ["state_topic"],
            "light": ["command_topic", "state_topic", "brightness_command_topic", "brightness_state_topic", "brightness_scale"],
            "number": ["command_topic", "state_topic", "min", "max", "step"], "button": ["command_topic"]}
    for topic, d in disc:
        comp, obj = topic.split("/")[1], topic.split("/")[3]
        tag = "%s/%s" % (comp, obj)
        for k in ["name", "unique_id", "availability_topic", "device"] + need.get(comp, []):
            if k not in d:
                problems.append("%s: missing %s" % (tag, k))
        if d.get("unique_id") in ids:
            problems.append("%s: duplicate unique_id" % tag)
        ids.add(d.get("unique_id"))
        if not d.get("device", {}).get("identifiers"):
            problems.append("%s: device has no identifiers" % tag)
        for k in ("state_topic", "command_topic", "availability_topic", "brightness_state_topic", "brightness_command_topic"):
            if k in d and not d[k].startswith(base + "/"):
                problems.append("%s: %s is outside the board's prefix" % (tag, k))
        if env and state is not None and comp == "light":
            on = env.from_string(d["state_value_template"]).render(value_json=state)
            if on not in ("ON", "OFF"):
                problems.append("%s: state template gave %r, expected ON/OFF" % (tag, on))
            try:
                v = float(env.from_string(d["brightness_value_template"]).render(value_json=state))
                if not 0 <= v <= d["brightness_scale"]:
                    problems.append("%s: brightness %s is outside 0..%s" % (tag, v, d["brightness_scale"]))
            except ValueError:
                problems.append("%s: brightness template did not give a number" % tag)
        if env and state is not None and "value_template" in d:
            out = env.from_string(d["value_template"]).render(value_json=state)
            if comp in ("switch", "binary_sensor") and out not in ("ON", "OFF"):
                problems.append("%s: template gave %r, expected ON/OFF" % (tag, out))
            if comp == "number":
                try:
                    v = float(out)
                    if not d["min"] <= v <= d["max"]:
                        problems.append("%s: current value %s is outside %s..%s" % (tag, v, d["min"], d["max"]))
                except ValueError:
                    problems.append("%s: template gave %r, expected a number" % (tag, out))
    for p in problems:
        print("PROBLEM:", p)
    print("%d problem(s)" % len(problems))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
