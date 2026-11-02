# Fancy2WLED

Make a **WLED** strip follow your **FancyLEDs Sync Box** (HDMI ambient light). A small ESP32-S3 board listens to the
box's wireless sync broadcast and relays the colours to your WLED, so the box's own LED strip is not needed to feed
your lights and no PC has to stay on.

* Runs entirely on an **M5Stack AtomS3 Lite** (about 8 USD). No cloud, no app, no PC.
* **Set up from your phone**: join the board's Wi-Fi, pick your network, find your WLED. Done.
* **Auto-calibration with your phone's camera**: point it at the TV and the LEDs; the relay measures the TV with nine
  squares, works out which LEDs it controls and where they are. LEDs along the TV's width are exact, the rest of the strip
  smooths out. Works for a strip along the wall behind the TV, around the TV, or something in between.
* Streams ~30 fps to WLED over DDP, with smoothing, brightness/gamma/saturation.

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
4. **WLED:** press **Find** (or type its address), then **Check this WLED**.
5. **Your LEDs:** say *where* they are (above the TV, below it, left, right, or all the way around), how long the run is (first LED +
   number of LEDs; e.g. the first 210 LEDs of the strip: first LED 0, length 210) and which part of the run covers the TV (e.g. LEDs
   83 to 163). Above the TV uses the *top* of the picture's colour map, below uses the *bottom*, left/right use that edge. Not sure
   where things are? Press **Find my LEDs** and drag the slider: a block of white LEDs lights up wherever you put it, with buttons to use
   that spot as the start or end of the run or of the TV. Whenever you change the run or the TV part, a few **red** LEDs light up on
   the strip where the TV starts and ends and a few **blue** ones where the run starts and ends (for about 8 seconds, or press
   *Show markers on the LEDs*), so you can see exactly which LEDs you are choosing.
6. **Check it:** press **Auto-calibrate with my phone** and follow the page ([details](docs/CALIBRATION.md)) to have the board measure the
   LEDs and the TV for you, or set it by hand and press **Start rainbow test**: red shows at the TV's left edge through to violet at its
   right edge, and every change (the +1/+10 buttons, *slide the TV part along the run*, LED numbers, where the LEDs are, look) updates
   live without being saved. Press **Save** when it looks right (stopping the test without saving discards the changes).

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

The board's button: **tap it** to turn the sync on / off (the LED flashes green for on, violet for off; paused shows steady violet).
**Hold it for 8 seconds** to factory-reset the board (Wi-Fi and settings; the LED blinks red after 3 s, and letting go before 8 s cancels).

## Home Assistant

Turn on **Home Assistant (MQTT)** on the setup page and the board appears in Home Assistant by itself as a *Fancy2WLED* device: a **Sync**
on/off switch, status sensors (Sync Box connected, WLED reachable, state, frame rate, Wi-Fi signal), the Look settings as number entities
and rainbow-test / restart buttons. The board connects *out* to your MQTT broker, so a firewall only needs to allow **board -> broker
(TCP 1883)**. Details, entity list, topics and troubleshooting: [docs/HOME_ASSISTANT.md](docs/HOME_ASSISTANT.md).

## Settings

| Setting | Meaning |
|---|---|
| WLED address | IP or `name.local` of your WLED |
| Where the LEDs are | above the TV (uses the top row of the picture's colour map), below it (bottom row), left or right of it (that column), all the way around it, or the phone-measured layout |
| First LED / length of the run | the part of the WLED strip that belongs to the TV lighting (the rest is left alone) |
| The part that covers the TV | the strip's own LED numbers where the TV starts and ends along a straight run. Along it the colours follow the picture exactly; before and after it they fade smoothly |
| Switch WLED on while relaying | WLED shows nothing while it is off |
| Set WLED brightness | WLED's own master brightness (the relay's colours are scaled by it); -1 leaves it alone |
| Put WLED back how it was | when the box goes quiet, restore WLED's previous on/off + brightness |
| Layout | calibrated table, a straight line, or around the TV |
| Brightness / gamma / saturation | 1.0 = copy the box exactly. The box tops out around 200/255 for white; raise brightness if you want full white |
| Smoothing over time | 0 = instant, 0.9 = slow fades (the box itself already smooths ~0.3 s) |
| Smooth beyond the TV | LEDs lined up with the TV follow the picture exactly; LEDs past its edges fade toward the picture's average colour, reaching it at the end of the strip. 100% = the whole tail, 0 = repeat the edge colour |

## Troubleshooting

**"Sync Box: nothing yet".** The box broadcasts to the local subnet. The relay must be on the same Wi-Fi *and* the same
subnet/VLAN. Broadcasts do not cross routers, and some access points / "client isolation" / "multicast-to-unicast" settings
drop them. Test with another computer on the same Wi-Fi: `python tools/sim_box.py --target <relay IP>` sends fake frames
straight to the relay so you can tell a network problem from a box problem.

**WLED does not change.** In WLED: *Config -> Sync Interfaces -> Receive UDP realtime* must be on (it is by default; DDP
needs it). Make sure no other software is streaming to the same WLED. Check **Check this WLED** shows the right LED count.

**Colours look different from the TV.** That is the box: its output is strongly non-linear (dark scenes go fully black,
white tops out near 200/255, green is yellowish). The relay copies it so your WLED matches what the box's own strip would
show. See [PROTOCOL.md](docs/PROTOCOL.md) and use the Look sliders to taste.

**The phone page says the connection is not private.** Expected: the relay makes its own HTTPS certificate (browsers
only allow camera access on HTTPS). Continue anyway; it never leaves your network.

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
node test/test_lint.js            # sources must be plain ASCII, LF, no BOM (non-ASCII shows up garbled on the device)
pio run -e atoms3                 # compile the firmware
```

`node test/mock_device.js` serves the web UI against a fake relay so you can work on it without hardware.

## How it works

* `web/` - the UI (setup, phone calibration, TV screen), compiled into the firmware.
* `src/core.cpp` - frame decoding, zone -> LED mapping, DDP packets (no Arduino dependencies, tested on a PC).
* `src/relay.cpp` - UDP in, smoothing, DDP out, WLED on/off handling, calibration and test pattern states.
* `src/webserver.cpp` - HTTP on :80 and HTTPS on :443 (self-signed key generated on first boot), JSON API.
* `src/mqtt_ha.cpp` - Home Assistant over MQTT (discovery, state, commands), runs in its own task.
* `tools/` - Python reference implementation, test-vector generator, a Sync Box simulator and `check_mqtt.py` (checks the board's MQTT discovery).

The image analysis for calibration runs in the phone's browser (`web/analysis.js`, `web/calibration.js`); the board only
lights LEDs on request and stores the resulting table.

## Licence

MIT, see [LICENSE](LICENSE).
