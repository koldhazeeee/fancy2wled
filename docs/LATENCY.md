# Why the lights lag behind the picture, and what you can do about it

Everything below was measured on one real setup (a FancyLEDs Sync Box, an AtomS3 Lite relay and a 600-LED WLED strip on a UniFi
access point). Your numbers will differ, but the shape of the problem is the same. Measure yours with the tools at the end.

## Where the time goes

When the picture changes colour, the lights follow after:

| Step | Delay | Can the relay change it? |
|---|---|---|
| 1. The Sync Box smooths its own output | **about 140 ms to half way, 350 ms to 90 %** | Yes: *Reaction boost* undoes it |
| 2. The broadcast reaches the relay over Wi-Fi | **about 50 ms on average** here (bursts every ~102 ms, see below) | No, it is the network |
| 3. The relay turns the frame into LED colours and sends it | **3 ms on average, 6 ms at worst** (it was 17 / 32 ms before the relay sent frames the moment they arrive) | Done |
| 4. The relay's packet crosses Wi-Fi to WLED | a few ms | No |
| 5. WLED clocks the frame out to the strip | up to one refresh, **~30 ms** for 600 LEDs (WLED measured 31-37 fps) | No, but see the WLED tips |
| 6. The relay's own *Smoothing over time* | about 45 ms to half way, 65 ms to 90 % at the default 0.5 | Yes: lower it |

The Sync Box is by far the biggest part. It does not broadcast the picture colour; it broadcasts a smoothed version: every
frame (about 14 a second) its output moves 40 % of the way to the real colour (`new = 0.6 * old + 0.4 * target`, measured over
thousands of frames, see [PROTOCOL.md](PROTOCOL.md)). That is why the box's own strip looks soft, and why mirroring it lags.

## Reaction boost

Because the filter is known, it can be run backwards: `target = y + 1.5 * (y - previous y)`. Setup page -> Look -> **Reaction
boost** (also a number in Home Assistant) blends from the box's own value (0 %) to the rebuilt target (100 %). A step from black to
red, measured at the relay's output with `tools/latency_test.py` (milliseconds after the change was sent):

| Reaction boost | 50 % of the way | 90 % | 98 % |
|---|---|---|---|
| 0 % (default, same as the box) | 120 | 357 | 567 |
| 60 % | 19 | 221 | 449 |
| 100 % | 8-28 | 77 | 154 |

The trade-off: the lights react like the picture does, not like the box's soft strip. Fast flashes look sharper, and a scene that
fades to black can drop to black a little early. 50-70 % is a good place to start; it is off by default so nothing changes until you
turn it up. It needs only the previous frame, so it adds no delay of its own.

## What the relay does to stay fast

* A Sync Box frame is turned into LED colours and sent **the moment it arrives** (it used to wait for the next 30 fps tick), and a
  tick every 33 ms keeps the smoothing moving until the lights reach the target.
* The smoothing is computed from elapsed time, not per tick, so the extra frame does not change how it feels.
* The main loop sleeps until a packet arrives instead of polling a thousand times a second.
* A frame that is identical to the last one is not sent again (a keep-alive every second stops WLED leaving realtime mode). A still
  picture costs about one packet a second instead of thirty.
* UDP uses plain sockets: no per-packet buffer allocation.

## The network: bursts instead of a steady stream

The box sends about 14 frames a second, evenly. With *Performance stats (debug)* on (see [HOME_ASSISTANT.md](HOME_ASSISTANT.md))
the relay's `gap_hist` shows how they actually arrive. On the measured network almost nothing arrived 20-80 ms apart:
about a third of the frames came back-to-back (under 5 ms) and most of the rest 80-120 ms apart, which is the access point's
beacon interval (102.4 ms). The same pattern showed on a second device, with the relay's Wi-Fi power-save off, so it happens before the
packets reach the relay.

A likely cause (not proven on every router): when any device on the Wi-Fi network is in power-save mode, the access point holds all
broadcast packets until its next beacon and then releases them together. If that is your case, things worth trying: a 2.4 GHz network
with only the Sync Box, the relay and WLED on it; checking your access point's broadcast / multicast / DTIM settings; or wiring the Sync Box
if your model allows it. The relay can do nothing about it, but it always uses the newest frame of a burst.

## WLED tips

* WLED sends every LED of its configured length on each refresh (about 30 microseconds each). 600 LEDs take ~18 ms; if only 210 of
  them are behind the TV, putting the TV part on its own output pin, or lowering the LED count in WLED if the rest is unused, makes every
  refresh faster.
* *Config -> LED Preferences -> Target refresh rate* should not be lower than the strip can do. *Config -> WiFi Setup -> Disable
  WiFi sleep* should be on (it is by default on ESP32).

## Delay to each target (setup page)

The status card at the top of the setup page has a collapsible **Delay to the lights** with one line per target, measured while the TV is on. It counts
from the moment a Sync Box frame reaches the board:

* **board**: the time from reading a frame to handing the packet that carries it for this target to the network (average, and the worst of the last
  5 seconds when it is more than twice the average). This is a measurement. The first target is sent first; each next one adds its own colour mixing and send.
* **network**: half of a probe's round trip to the target. A probe goes out every 10 s (a WLED: how long a TCP connection to its port 80 takes to open; the
  board never sends it anything else) and the page shows the average of the last 20, with how many went unanswered. Half a round trip is an
  assumption (both ways take equally long), not a measurement of one way.
* **strip refresh** (a WLED): a model. WLED clocks the strip out at about 30 microseconds per LED over its whole configured length, and a frame that arrives
  waits on average half a refresh for the next one (the refresh rate is read from WLED now and then). It is not a measurement of the strip.
* **a Govee light** has no frame acknowledgement, so only the board's share is measured, plus how long the light takes to answer a status question
  (once every 30 s). That time includes the light's own processing, so it is not added up. A Govee light takes at most about 12 frames a second, so a new
  frame can wait up to 80 ms for its turn; this shows in the board's share (28-38 ms on average on the two lights it was tried on).

The same numbers are in `/api/status` under each target's `lat`. On the setup this was developed on, the TV strip showed board 4.5 ms + network 9 ms +
strip refresh 42 ms = about 56 ms, and the Cabinet about 52 ms.

Not included: the Sync Box's own smoothing (above), and the Wi-Fi hop from the box to the board. Neither can be seen from the board: the frames carry no
timestamp, and the box and the board do not share a clock.

## Measuring it yourself

* **Delay inside the relay and the shape of the Wi-Fi delivery**: switch on *Performance stats (debug)*. `Delay to WLED` is the
  time from a Sync Box frame arriving to the packet that carries it leaving; `gap_hist` in the `<prefix>/perf` message is the
  histogram of the gaps between Sync Box frames.
* **From the Sync Box's broadcast to a WLED strip really changing**: `python tools/latency_e2e.py fancy2wled.local` (play something moving while it
  runs, 40 s by default). It listens to the box's broadcast and, over the WebSocket the WLED web page uses for its live view, to each WLED strip, on this
  computer's clock, and reports the delay at which the two match best (cross-correlation of the changes in brightness; it also prints the result for each
  third of the run, so you can see how steady it is). It sends nothing and changes nothing. A negative number means the strip is ahead of the broadcast
  (the reaction boost looks forward). Limits: a Wi-Fi adapter in power-save can hold broadcasts back, which would make the delay look longer than the relay
  sees it; the live view is about 20 frames a second, so the result is an average over many frames, not one frame's delay; a Govee light has no live view.
  On the setup it was developed on it gave about 65 ms for the TV strip and 50 ms for the Cabinet, with the board's own numbers adding up to about 56
  and 52 ms.
* **End to end at the relay's output**: `python tools/latency_test.py fancy2wled.local`. It plays the part of the Sync Box and of WLED,
  sends black/red steps shaped like the real box's output and prints how long the output takes to follow. Run it at different
  *Reaction boost* and *Smoothing over time* settings. It puts your settings back when it finishes.
