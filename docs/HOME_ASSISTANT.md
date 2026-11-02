# Home Assistant

Fancy2WLED talks to Home Assistant over **MQTT with discovery**: turn it on once and the board shows up in Home Assistant as a
**Fancy2WLED** device with its entities, no YAML and no custom integration. The board connects *out* to your MQTT broker (for
example Home Assistant's *Mosquitto broker* add-on).

## Set up

1. In Home Assistant you need an MQTT broker and the MQTT integration (*Settings -> Devices & services -> MQTT*). With the
   Mosquitto add-on, create a user for the board (or use an existing one).
2. On the board's setup page (`http://fancy2wled.local`) open **Home Assistant (MQTT)**, tick *Connect to MQTT*, enter the broker's
   address (use its IP), the port (1883), the user name and password, and press **Save and connect**. The status line says
   *Connected to the broker* when it works.
3. In Home Assistant the device appears by itself under *Settings -> Devices & services -> MQTT -> Devices -> Fancy2WLED*.

**Firewall / VLANs.** The board makes the connection, so allow **board -> broker, TCP 1883** (or 8883 is not supported). If the board
is on an IoT network and Home Assistant on another, that rule is the only one needed; Home Assistant never has to call the board.
Give the board a DHCP reservation if you like, but nothing depends on its address: Home Assistant only talks to the broker.

## What you get

| Entity | What it does |
|---|---|
| **Sync** (light) | On / off pauses and resumes the sync (paused: nothing is streamed and WLED is handed back to how it was; same as the board's button tap). The **brightness slider** is the Look brightness, 0-100 % (100 % = copy the box exactly). Moving the slider while it is off turns the sync back on |
| **State** (sensor) | relaying, waiting for the Sync Box, paused, needs setup, calibrating, showing markers, showing test pattern |
| **Sync Box** (connectivity) | the board is receiving the Sync Box |
| **WLED** (connectivity) | WLED was reachable the last time the board talked to it |
| **Relaying** (running) | colours are being streamed to WLED right now |
| **Sync Box frame rate**, **Wi-Fi signal**, **Uptime** | sensors (the last two are diagnostic) |
| **Gamma**, **Saturation** (0-200 %), **Smoothing over time** (0-90 %), **Smooth beyond the TV** (0-100 %) | the *Look* settings, saved on the board |
| **Rainbow test**, **Stop test**, **Restart** (buttons) | the same as the setup page |

The entities go *unavailable* when the board loses its connection to the broker (it announces `offline`, and the broker publishes the
same if the board vanishes unexpectedly).

Layout (where the LEDs are, the run and the TV part) is set on the board's own page, not in Home Assistant.

## Automation ideas

* Turn the sync off when the TV turns off: trigger on your TV's `media_player` going `off`, action `light.turn_off` on *Fancy2WLED Sync*
  (WLED goes back to its own state, e.g. off or a scene).
* Dim the ambient light in the evening: `light.turn_on` on *Fancy2WLED Sync* with `brightness_pct: 60` after sunset.
* Start a movie scene: `light.turn_on` on *Fancy2WLED Sync* (add `brightness_pct: 100` if you want).

## Topics (for other tools)

All under `<prefix>` (default `fancy2wled/<last 4 digits of the board id>`, changeable on the setup page):

| Topic | Direction | Payload |
|---|---|---|
| `<prefix>/status` | board -> broker (retained, last-will) | `online` / `offline` |
| `<prefix>/state` | board -> broker | JSON: `state, enabled, box_connected, box_fps, wled_ok, wled_leds, rssi, uptime, brightness, gamma, saturation, smoothing, smooth_out, relaying` |
| `<prefix>/sync/set` | -> board | `ON` / `OFF` (`1`/`0`, `true`/`false`) |
| `<prefix>/look/<name>/set` | -> board | a number (the light's brightness slider uses `brightness`): `brightness` (0-100), `gamma` (0.6-2.4), `saturation` (0-200), `smoothing` (0-90), `smooth_out` (0-100) |
| `<prefix>/button/<name>/press` | -> board | `PRESS` for `rainbow`, `stop_test`, `restart` |
| `homeassistant/<component>/<id>/<object>/config` | board -> broker (retained) | discovery messages |

Malformed or out-of-range values are ignored. `python tools/check_mqtt.py <broker>` connects to a broker and checks that the discovery
messages are complete and that their templates render against the live state.

## Troubleshooting

* *Not connected: Cannot reach the broker* - wrong address/port, the broker is not running, or a firewall blocks board -> broker TCP.
* *The broker refused the user name / password* - check them (the password is never shown again; type it again to change it).
* Connected but no device in Home Assistant - the discovery prefix must match Home Assistant's (default `homeassistant`), and the MQTT
  integration must be set up.
* Upgrading from an older version: the board removes the old *Sync* switch and *Brightness* number from Home Assistant by itself and adds the *Sync* light (automations that used `switch.fancy2wled_sync` or `number.fancy2wled_brightness` need to use `light.fancy2wled_sync` instead).
* Entities show old values after you change the topic prefix - delete the old device in Home Assistant (*MQTT -> Fancy2WLED -> Delete*).
