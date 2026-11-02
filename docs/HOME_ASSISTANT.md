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
| **Gamma**, **Saturation** (0-200 %), **Smoothing over time** (0-90 %), **Reaction boost** (0-100 %, see [LATENCY.md](LATENCY.md)), **Smooth beyond the TV** (0-100 %) | the *Look* settings, saved on the board |
| **One light per target** (named after the target, for example *Cabinet*) | on / off = stream to that target or not; the brightness slider is its brightness compared with the others, 0-100 % (the setup page goes up to 150 %). They appear and disappear as you add, rename and remove targets |
| **Rainbow test**, **Stop test**, **Restart** (buttons) | the same as the setup page |

The entities go *unavailable* when the board loses its connection to the broker (it announces `offline`, and the broker publishes the
same if the board vanishes unexpectedly).

Layout (where the LEDs are, the run and the TV part) is set on the board's own page, not in Home Assistant.

## Performance stats (debug)

Off by default. Tick **Performance stats (debug)** in the MQTT card on the setup page (and press *Save and connect*) to see how hard the
board is working. Every 5 seconds it publishes to `<prefix>/perf` and Home Assistant gets these **diagnostic** sensors:

| Sensor | Meaning |
|---|---|
| **CPU core 0 / core 1 load** | % of the time each core was busy (core 0 mostly runs Wi-Fi; core 1 the relay loop) |
| **Main loop busy / longest pass** | % of the time the relay loop spends working, and its slowest single pass in the window (a stall detector) |
| **Frame work (average / longest)** | time to mix, smooth and send one frame to WLED |
| **Frames sent to WLED** | frames per second actually sent (a still picture sends only a keep-alive each second) |
| **Delay to WLED (average / longest)** | time from a Sync Box frame arriving to the first packet carrying it leaving for WLED |
| **Sync Box frame gap (longest)** | the longest time between two Sync Box frames in the window (the `gap_hist` field of the `perf` message shows the whole spread) |
| **Free memory / Lowest free memory since boot / Largest free memory block** | heap; the lowest value shows the worst moment since boot, the largest block shows fragmentation |
| **Main task / MQTT task stack left** | stack bytes those tasks have never used (a small number would mean a risk of overflow) |

Typical values on an AtomS3 Lite relaying a Sync Box to 210 LEDs: core 0 about 4 %, core 1 about 9 %, main loop busy about 6 %, frame work
about 1.1 ms, delay to WLED about 3 ms, about 155 KB of free memory.

**Switched off, nothing is measured.** The CPU sampling timers are stopped, the counters do not run, nothing is published to `<prefix>/perf`,
and the sensors are removed from Home Assistant again. While on it costs a little CPU (two small timer interrupts about 1400 times a second),
so switch it off when you are done. Measuring only happens while the board is connected to the broker.

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
| `<prefix>/state` | board -> broker | JSON: `state, enabled, box_connected, box_fps, wled_ok (all enabled targets), wled_leds (all targets), rssi, uptime, brightness, gamma, saturation, smoothing, boost, smooth_out, relaying, targets` (a list of `{on, trim, ok}`, one per target) |
| `<prefix>/sync/set` | -> board | `ON` / `OFF` (`1`/`0`, `true`/`false`) |
| `<prefix>/look/<name>/set` | -> board | a number (the light's brightness slider uses `brightness`): `brightness` (0-100), `gamma` (0.6-2.4), `saturation` (0-200), `smoothing` (0-90), `boost` (0-100), `smooth_out` (0-100) |
| `<prefix>/target/<n>/set` | -> board | `ON` / `OFF`: stream to target `n` (0 = the first) or not |
| `<prefix>/target/<n>/trim/set` | -> board | a number 0-150: that target's brightness in % |
| `<prefix>/button/<name>/press` | -> board | `PRESS` for `rainbow`, `stop_test`, `restart` |
| `homeassistant/<component>/<id>/<object>/config` | board -> broker (retained) | discovery messages |

Malformed or out-of-range values are ignored. `python tools/check_mqtt.py <broker>` connects to a broker and checks that the discovery
messages are complete and that their templates render against the live state.

## Troubleshooting

* *Not connected: Cannot reach the broker* - wrong address/port, the broker is not running, or a firewall blocks board -> broker TCP.
* *The broker refused the user name / password* - check them (the password is never shown again; type it again to change it).
* Connected but no device in Home Assistant - the discovery prefix must match Home Assistant's (default `homeassistant`), and the MQTT
  integration must be set up.
* Upgrading from an older version: the board removes the old *Sync* switch and *Brightness* number (and the per-target switches and brightness numbers) from Home Assistant by itself and adds the *Sync* light and a light per target (automations that used `switch.fancy2wled_sync` or `number.fancy2wled_brightness` need to use `light.fancy2wled_sync` instead).
* Entities show old values after you change the topic prefix - delete the old device in Home Assistant (*MQTT -> Fancy2WLED -> Delete*).
