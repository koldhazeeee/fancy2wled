# Fancy2WLED

Make a **WLED** strip follow your **FancyLEDs Sync Box** (HDMI ambient light). A small ESP32-S3 board listens to the
box's wireless sync broadcast and relays the colours to your WLED, so the box's own LED strip is not needed to feed
your lights and no PC has to stay on.

* Runs entirely on an **M5Stack AtomS3 Lite** (about 8 USD). No cloud, no app, no PC.
* **Set up from your phone**: join the board's Wi-Fi, pick your network, find your WLED. Done.
* **Up to four targets**: a strip behind the TV, a cabinet light, a second WLED. Each has its own LEDs, its own part of the picture
  to follow (the top, bottom, left or right edge, or all the way around) and its own brightness. One WLED can serve several targets.
* **See what each target follows**: a *picture map* on the setup page shows the TV as the Sync Box's 6 by 4 grid with the zones every target pulls
  from, each target in its own colour and shaded by how many of its LEDs use the zone. It redraws as you edit, before you save.
* **Updates over Wi-Fi**: after the first USB flash, upload `firmware.bin` on the setup page or push it with PlatformIO. Off until you set an update password;
  a new firmware that fails to start is undone by the board itself ([docs/OTA.md](docs/OTA.md)).
* **Follows the TV's power**: the Sync Box sends no TV-off signal, but its picture goes completely black when the TV is switched off (including by CEC). After
  a delay you choose (15 s by default) the WLEDs are handed back to how they were, or switched off, and they return with the first colour. Home Assistant gets a *TV on* sensor.
* **Find your LEDs by sight**: a block of lit LEDs you slide along the strip, and red / blue markers that show where the run and the TV
  part start and end.
* Streams ~30 fps to WLED over DDP, with smoothing, brightness/gamma/saturation and a **reaction boost** that cuts the delay
  ([why the lights lag](docs/LATENCY.md)).
* **Tells you when the box changes**: if a Sync Box firmware update changes what it broadcasts, the relay says so (setup page, red LED, Home Assistant) and
  can give you a diagnostics file, instead of just looking like a switched-off box ([docs/RESILIENCE.md](docs/RESILIENCE.md)).
* *Optional:* **auto-calibration with your phone's camera** (a separate build, `atoms3-phonecal`; see [docs/CALIBRATION.md](docs/CALIBRATION.md)).

> **Unofficial.** This project is not affiliated with or endorsed by FancyLEDs, Tuya or WLED. It was developed against a
> FancyLEDs "3 Port 2.1 Fancy Sync Box" (FSB3P2.1) by observing traffic on the author's own network; other models or
> firmware may differ ([protocol notes](docs/PROTOCOL.md)).

## What you need

* A **Sync Box** (powered and set to its normal screen-sync mode) and a **WLED** device with LEDs, on the **same network**
  (same Wi-Fi, same subnet/VLAN as the box: it talks by broadcast).
* An **M5Stack AtomS3 Lite** (any ESP32-S3 works with small changes to `platformio.ini` and the LED/button pins).
* A computer with [PlatformIO](https://platformio.org/) once, to put the firmware on the board.

## Quick start

1. **Flash.** Plug the AtomS3 Lite into USB and run
   ```
   pio run -e atoms3 -t upload
   ```
   (or `python -m platformio run -e atoms3 -t upload`).
2. **Join its Wi-Fi.** The LED blinks purple. On your phone connect to **`Fancy2WLED-Setup`**; the setup page opens by
   itself (otherwise browse to `http://192.168.5.5`). Choose your Wi-Fi, enter the password. The board restarts and joins it.
3. **Open the setup page.** From any device on that network: **`http://fancy2wled.local`** (or the IP your router shows).
4. **WLED:** press **Find** (or type its address), then **Check this WLED**. The **Targets** card at the top picks which target you are
   editing; press **Add a target** for each further strip or WLED (up to four) and give it a name.
5. **Your LEDs** (for the target you picked; the **Picture map** above shows which part of the screen each target will follow): say *where* they are (above the TV, below it, left, right, or all the way around), how long the run is (first LED +
   number of LEDs; e.g. the first 210 LEDs of the strip: first LED 0, length 210) and which part of the run covers the TV (e.g. LEDs
   83 to 163). Above the TV uses the *top* of the picture's colour map, below uses the *bottom*, left/right use that edge. Not sure
   where things are? Press **Find my LEDs** and drag the slider: a block of white LEDs lights up wherever you put it, with buttons to use
   that spot as the start or end of the run or of the TV. Whenever you change the run or the TV part, a few **red** LEDs light up on
   the strip where the TV starts and ends and a few **blue** ones where the run starts and ends (for about 8 seconds, or press
   *Show markers on the LEDs*), so you can see exactly which LEDs you are choosing.
6. **Check it:** press **Start rainbow test**: red shows at the TV's left edge through to violet at its right edge on the target you are
   editing, and every change (the +1/+10 buttons, *slide the TV part along the run*, LED numbers, where the LEDs are, look) updates
   live without being saved. Press **Save** when it looks right (stopping the test without saving discards the changes).
   (A build with the phone-camera calibration also offers **Auto-calibrate with my phone**: `pio run -e atoms3-phonecal -t upload`.)

The LED turns **green** while it is relaying. That is it: your WLED now follows the picture.

### What the board's LED means

| Colour | Meaning |
|---|---|
| purple, blinking | setup access point (waiting for you to pick a Wi-Fi) |
| blue, blinking | joining Wi-Fi |
| blue | connected, waiting for the Sync Box |
| yellow | WLED not set up yet |
| violet | sync paused (button, setup page or Home Assistant) |
| green | relaying |
| white | calibrating / showing the test pattern |
| red, blinking | the Sync Box is sending, but its frames cannot be read |

The board's button: **tap it** to turn the sync on / off (the LED flashes green for on, violet for off; paused shows steady violet).
**Hold it for 8 seconds** to factory-reset the board (Wi-Fi and settings; the LED blinks red after 3 s, and letting go before 8 s cancels).

## Home Assistant

Turn on **Home Assistant (MQTT)** on the setup page and the board appears in Home Assistant by itself as a *Fancy2WLED* device: a **Sync**
light (on/off pauses the sync, the brightness slider is the brightness), status sensors (Sync Box connected, WLED reachable, state, frame rate, Wi-Fi signal), the Look settings as number entities,
a light (on/off and brightness) for each target, and rainbow-test / restart buttons. The board connects *out* to your MQTT broker, so a firewall only needs to allow **board -> broker
(TCP 1883)**. Details, entity list, topics and troubleshooting: [docs/HOME_ASSISTANT.md](docs/HOME_ASSISTANT.md).

## Back up, or move to another board

At the bottom of the setup page, **Export config** downloads `fancy2wled-config.json` with all your targets (WLED addresses, runs, layouts, any phone
calibration), Look settings and MQTT settings. To move to a new ESP32: flash it, join its setup Wi-Fi and enter your Wi-Fi details as
usual, then open the setup page and press **Import config**. The page shows a summary and asks before it replaces anything.

* The **Wi-Fi name and password are never in the file**, so it is safe to keep; a new board always joins Wi-Fi first.
* The **MQTT password** is only included if you tick the box before exporting (then treat the file like a password). Without it, type the
  password again in the MQTT card after importing.
* An import is all or nothing: a file that is not a Fancy2WLED config, comes from a newer version, or has an invalid value is refused
  with a message and nothing on the board changes.
* The file is plain JSON (`format`, `version`, `config`, optional `table`), so you can also keep it in version control or edit it.

## Settings

| Setting | Meaning |
|---|---|
| WLED address | IP or `name.local` of your WLED (a pasted `http://wled-ceiling.local/` is cleaned up). **Prefer the name** (set it in WLED under Config > WiFi Setup > mDNS address): the board looks it up again every minute, so a WLED that gets a new IP from your router is found again on its own. The page shows which address the name points to. The *Find* button fills in the name. |
| Picture map | not a setting: the TV as a 6 by 4 grid with the zones each target follows highlighted in its colour, shaded by how many of its LEDs use each zone. It follows unsaved edits (the board does the counting with the same code that drives the lights; `POST /api/usage`) |
| When the TV is off | what to do when the picture has been completely black for the chosen number of seconds (3-600, default 15): **hand the WLEDs back** as they were before sync started (default), **switch them off**, or do nothing (the *TV on* sensor still works). They return with the first frame that has any colour. A long black screen counts as "TV off" too, so raise the delay if that bothers you. Inferred from the picture, not a signal from the box: see [PROTOCOL.md](docs/PROTOCOL.md) |
| Firmware updates | off until an **update password** (8-64 characters) is set; then tick *Allow firmware updates over Wi-Fi* and upload `firmware.bin` here, or run `pio run -e atoms3-ota -t upload` with `FANCY2WLED_OTA_PASSWORD` set. Changing the password needs the current one; a config file cannot set it. Details, safety rules and rollback: [docs/OTA.md](docs/OTA.md) |
| Targets | up to four runs of LEDs, each on a WLED of its own or sharing one. Each has a name, an on/off switch, its WLED, its run, where it is (which edge of the picture it follows) and a brightness trim. The Look settings below are shared |
| Where the LEDs are | above the TV (uses the top row of the picture's colour map), below it (bottom row), left or right of it (that column), all the way around it, or the phone-measured layout |
| First LED / length of the run | the part of the WLED strip that belongs to the TV lighting (the rest is left alone) |
| The part that covers the TV | the strip's own LED numbers where the TV starts and ends along a straight run. Along it the colours follow the picture exactly; before and after it they fade smoothly |
| Switch WLED on while relaying | WLED shows nothing while it is off |
| Set WLED brightness | WLED's own master brightness (the relay's colours are scaled by it); -1 leaves it alone |
| Put WLED back how it was | when the box goes quiet, restore WLED's previous on/off + brightness |
| Layout | a straight line, around the TV, or (with the phone-calibration build) a table measured by the phone |
| Brightness / gamma / saturation | 1.0 = copy the box exactly. The box tops out around 200/255 for white; raise brightness if you want full white |
| Smoothing over time | 0 = instant, 0.9 = slow fades (the box itself already smooths ~0.3 s) |
| Reaction boost | 0 = follow the box exactly (default), 100 = undo the box's own smoothing so the lights react as fast as the picture. Try 50-70. See [docs/LATENCY.md](docs/LATENCY.md) |
| Smooth beyond the TV | LEDs lined up with the TV follow the picture exactly; LEDs past its edges hold the colour of the corner zone at their end of the strip (for a ceiling strip: the top-left zone on the left, the top-right zone on the right), so a colour elsewhere on screen never reaches them. This setting only softens the hand-over right next to the TV's edge 100% = the whole tail, 0 = repeat the edge colour |

## Troubleshooting

**"Sync Box: frames not understood" (red), or the LED blinks red.** Data is arriving from the box but none of it can be read: its firmware probably changed
what it sends. Press **Download diagnostics** on the setup page and attach the file to an issue. What to do before and after a box update, and how to compare
captures: [docs/RESILIENCE.md](docs/RESILIENCE.md).

**The lights lag behind the picture.** Most of that delay is the Sync Box smoothing its own colours, then the Wi-Fi network delivering
its broadcast in bursts. Turn up **Reaction boost** (Look) and see [docs/LATENCY.md](docs/LATENCY.md) for where the time goes, what to change
on your network, and how to measure it (`tools/latency_test.py`).

**"Sync Box: nothing yet".** The box broadcasts to the local subnet. The relay must be on the same Wi-Fi *and* the same
subnet/VLAN. Broadcasts do not cross routers, and some access points / "client isolation" / "multicast-to-unicast" settings
drop them. Test with another computer on the same Wi-Fi: `python tools/sim_box.py --target <relay IP>` sends fake frames
straight to the relay so you can tell a network problem from a box problem.

**WLED does not change.** In WLED: *Config -> Sync Interfaces -> Receive UDP realtime* must be on (it is by default; DDP
needs it). Make sure no other software is streaming to the same WLED. Check **Check this WLED** shows the right LED count.

**Colours look different from the TV.** That is the box: its output is strongly non-linear (dark scenes go fully black,
white tops out near 200/255, green is yellowish). The relay copies it so your WLED matches what the box's own strip would
show. See [PROTOCOL.md](docs/PROTOCOL.md) and use the Look sliders to taste.

**The phone page says the connection is not private.** (Only in the `atoms3-phonecal` build.) Expected: the relay makes its own HTTPS certificate
(browsers only allow camera access on HTTPS). Continue anyway; it never leaves your network.

**Setup page says "Wi-Fi can only be changed from the setup access point".** Hold the button for 8 seconds to reset, then
join `Fancy2WLED-Setup` again.

## Security

There is no password: anyone on your Wi-Fi can open the setup page, as with WLED itself. Do not expose it to the internet.
The setup access point is open while it is waiting for Wi-Fi details (it disappears once configured). POST requests must be
`application/json`, which stops other websites from changing settings through your browser.

## Building and testing

```
python test/run_core_tests.py     # C++ core vs golden vectors from the Python reference (needs g++/clang++ or `pip install ziglang`)
node test/test_analysis.js        # calibration maths + a simulated phone camera (needs Node 18+)
node test/test_pages.js           # loads the real web pages in headless Edge/Chrome against test/mock_device.js
python test/test_capture_tools.py  # the capture / firmware-change tools
node test/test_lint.js            # sources must be plain ASCII, LF, no BOM (non-ASCII shows up garbled on the device)
pio run -e atoms3                 # compile the firmware
pio run -e atoms3-phonecal        # ... with the phone-camera calibration and the secure web server it needs
```

`node test/mock_device.js` serves the web UI against a fake relay so you can work on it without hardware.

**Releases.** Bump `FWB_VERSION` in `platformio.ini`, commit, then `git tag v0.2.0 && git push origin v0.2.0` (the tag must match the version). The `release` workflow
builds both firmwares and publishes a GitHub release with `fancy2wled-atoms3-<version>.bin`, `fancy2wled-atoms3-phonecal-<version>.bin` and `SHA256SUMS.txt`: the file
to choose for *Update firmware* on the setup page ([docs/OTA.md](docs/OTA.md)). Every push also uploads the same files as a build artifact on its Actions run.

## How it works

* `web/` - the UI (setup; with `atoms3-phonecal` also the phone calibration and the TV screen), compiled into the firmware.
* `src/core.cpp` - frame decoding, zone -> LED mapping, DDP packets (no Arduino dependencies, tested on a PC).
* `src/relay.cpp` - UDP in (sleeps until a packet arrives), reaction boost, then for every target: layout, time-based smoothing, brightness trim, DDP out (identical frames are not resent). Turning WLEDs on / handing them back runs in a background task so a missing WLED never delays the others. Also the LED finder, markers and test pattern.
* `src/webserver.cpp` - HTTP on :80 (and, in the phone-calibration build, HTTPS on :443 with a self-signed key generated on first boot), JSON API.
* `src/mqtt_ha.cpp` - Home Assistant over MQTT (discovery, state, commands), runs in its own task.
* `src/perf.cpp` - optional performance statistics (CPU load per core, memory, frame timing), only active when switched on.
* `tools/` - Python reference implementation, test-vector generator, a Sync Box simulator, `check_mqtt.py` (checks the board's MQTT discovery), `latency_test.py` (measures how fast the output follows a step), `capture_frames.py` and `check_capture.py` (record the box's broadcast and compare captures, see [docs/RESILIENCE.md](docs/RESILIENCE.md)).

(Phone-calibration build:) the image analysis runs in the phone's browser (`web/analysis.js`, `web/calibration.js`); the board only
lights LEDs on request and stores the resulting table.

## Licence

MIT, see [LICENSE](LICENSE).
