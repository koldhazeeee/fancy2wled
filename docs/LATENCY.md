# Why the lights lag behind the picture, and what you can do about it

Everything below was measured on one real setup (a FancyLEDs Sync Box, an AtomS3 Lite relay and a 600-LED WLED strip on a UniFi
access point). Your numbers will differ, but the shape of the problem is the same. Measure yours with the tools at the end.

## Where the time goes

When the picture changes colour, the lights follow after:

| Step | Delay | Can the relay change it? |
|---|---|---|
| 1. The Sync Box smooths its own output | **about 140 ms to half way, 350 ms to 90 %** at its default speed of 40 %; **none at 100 %** | Yes: set the box's *Speed of Diffusion* to 100 %, or *Reaction boost* undoes it |
| 2. The broadcast reaches the relay over Wi-Fi | **about 50 ms on average** here (bursts every ~102 ms, see below) | No, it is the network |
| 3. The relay turns the frame into LED colours and sends it | **3 ms on average, 6 ms at worst** (it was 17 / 32 ms before the relay sent frames the moment they arrive) | Done |
| 4. The relay's packet crosses Wi-Fi to WLED | a few ms | No |
| 5. WLED clocks the frame out to the strip | up to one refresh, **~30 ms** for 600 LEDs (WLED measured 31-37 fps) | No, but see the WLED tips |
| 6. The relay's own *Smoothing over time* | about 45 ms to half way, 65 ms to 90 % at the default 0.5 | Yes: lower it |

The Sync Box is by far the biggest part. It does not broadcast the picture colour; it broadcasts a smoothed version: every
frame (about 14 a second) its output moves 40 % of the way to the real colour (`new = 0.6 * old + 0.4 * target`, measured over
thousands of frames, see [PROTOCOL.md](PROTOCOL.md)). That is why the box's own strip looks soft, and why mirroring it lags.

## Best: switch the box's smoothing off

The box app has **Speed of Diffusion** (the diffusion mode next to it changes nothing in the broadcast). Measured with `tools/sync_probe` (a laptop's HDMI
into the box, patterns of known timing, each change looked at on its own):

| Speed of Diffusion | A change rises from 10 % to 90 % in | Half of a change is shown after | 90 % after |
|---|---|---|---|
| 40 % | 210-300 ms, with half-way packets in nearly every change | 255-280 ms | 445-465 ms |
| **100 %** | **0 ms, no half-way packets** | **204 ms** | **204 ms** |

(The times include the laptop's own output and a Wi-Fi link that delivers broadcasts in bursts; compare the two rows, not the absolute numbers.) At 100 % the
box broadcasts the real picture, one packet after the other, so there is nothing left to undo. Set **Box speed** on the setup page to the same number as in
the box's app, and leave *Reaction boost* at 0 (it does nothing at a box speed of 100). *Motion smoothing* below then does the smoothing, once, in the
relay.

## Reaction boost

If you keep the box slower than 100 %, the smoothing can be undone in software. **Box speed** (Look) must match the box's app; the default 40 % is the box's
default.

Because the filter is known, it can be run backwards: `target = y + k * (y - previous y)`, with k = 1.5 at a box speed of 40 % (k = q^m / (1 - q^m) for a speed s, q = 1 - s, m = the packets' smoothing steps apart). Setup page -> Look -> **Reaction
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

Packets that Wi-Fi drops are handled by counting the steps from the gap between the received packets (a gap of 200 ms held three steps). Tested on
recordings made at 40 %: the undone steps rise instantly like a box at 100 % does, and at 100 % boost almost every packet is rebuilt to within a few percent.
One case cannot be seen in the stream: when the packet just before a change is lost, the first packet after it already holds two steps but has a normal gap,
so that one frame overshoots (up to about 60 % of the change at boost 100, about 25 % at boost 60, which is why 50-70 is the good place to start).

## Does Box speed match the box? (measured from the picture)

The relay can check the number you entered for **Box speed** against the box itself, from its packets alone. With the speed s set in the box's app, every packet moves each colour
channel a fraction s of the way to the new picture, so right after a sudden change (a scene cut, a flash) the second step is (1 - s) times the first: 0.6 times at 40 %, none at
100 %. Whenever at least 6 channels that were still start to move together, the relay takes the slope of the second step on the first over all of them (a least squares fit,
which averages out the rounding of the colours) as an estimate of 1 - s.

Test patterns give clean cuts, but **real video keeps moving during the second step**, which pushes some estimates anywhere (from -2.8 to 1.5 in a recording at 75 %). The clean
changes all land on the same value, the polluted ones scatter. So the answer is a **vote**, not an average: the latest 24 estimates vote for their bin of 5 % (the box's speeds
are multiples of 5), and the winner is the answer once 10 estimates exist. It counts as measured only when it has at least 5 votes, a quarter of all of them, and twice the
runner-up's; otherwise the page says no value stands out yet and keeps trying. What the page *shows* is held steady: an answer appears once the vote has been sure of it 3 estimates in a row,
and a shown answer is replaced by a different one only after 5 in a row, because on a live picture the vote flickers between sure, unsure and a neighbouring value while the
true speed rarely changes. (A first version used the median of the estimates with a limit on their spread. On normal video at
75 % that was wrong in 10 of 82 windows and undecided in 53; the vote was never wrong and undecided in 9 of 31.)

* **Only clear cases count.** A change counts when the channel was still before it, its first step is at least 24 levels, nothing near black is involved (the box cuts off
  near black), and no packet was lost within the four it spans.
* **Checked against the real box** (the box's app set to the speed shown):

  | box app | what was on screen | measured | sudden changes used |
  |---|---|---|---|
  | 40 % (three recordings, `tools/sync_probe`) | test patterns | 40 % in all three | 15, 18 and 22 |
  | 100 % (two recordings) | test patterns | 100 % in one, "not known yet" in the other (4 changes) | 10 and 4 |
  | 75 % (90 seconds, `tools/capture_frames.py`) | a TV programme | 75 % in 22 of 31 windows, never anything else (undecided in the rest) | 40 in all |

  A simulated box gives the speed that was set at 25, 40, 55, 70, 85 and 100 %, also with 5 to 30 % of the packets lost. An excerpt of each of the three kinds of real
  recording is part of the automated tests (`tools/reference/data/`: only the colours of the 24 zones), next to an independent Python version of the calculation. The 75 %
  vote was tuned on that one recording, so more recordings of other speeds and programmes are welcome.
* **Limits.** It needs sudden changes: a film with no cuts, or a still picture, gives no answer yet; with busy video it takes a minute or two to collect enough clear ones
  (about 0.4 usable changes a second in the 75 % recording). Below about 20 % the first step of any change is smaller than the measuring threshold, so it says "not known" instead of
  guessing. After you change the speed in the box's app the old votes have to be outvoted, which takes up to a couple of minutes of video.
* **Advice only.** Nothing the relay does with the picture depends on it. The setup page shows "Measured from the Sync Box" under *Box speed*, and the *Settings check* says
  so when the measurement is 10 % or more away from the number you entered (a *Fix* when Reaction boost is on, since it then undoes the wrong amount of smoothing; a *Tip*
  otherwise). The numbers are in the status (`/api/status`, `box.speed`: `pct`, `agree` votes of `n`, `mixed`, `changes`).

## Motion smoothing and blur

The box sends about 14 frames a second but a new picture only about every 100 ms, and the frames reach the board unevenly (see below), so a strip that jumps to
each new picture moves in steps, and with *Zone edges* at 100 whole blocks of LEDs change at once. Two settings smooth that:

* **Motion smoothing** (Look, 0-100): instead of jumping to each picture the lights slide from the previous picture to the newest one, shown that share of one
  picture interval behind the newest (at 100: one interval, about 100 ms). Each picture is placed on a slowly adjusting clock, so bursts and gaps do not make
  the motion lurch; two pictures within 25 ms of each other count as one. While it is on it replaces *Smoothing over time* and *Reaction boost* (both would
  fight it). On the recordings of real box output in `decoded/` it is about **2.7 times smoother** than the default pipeline and about 6 times smoother than
  holding each picture (average change of speed between 30 fps output frames: 0.20 against 0.54 and 1.20).
  **It costs delay.** Against the default smoothing (0.5, no boost) sliding over about 100 ms is comparable (about 50 ms behind on average, against about
  48 ms). Against a setup that uses *Reaction boost* it is later: a step in the picture reaches half way in about 83 ms and 90 % in about 130 ms on a WLED
  strip, against about 20-30 ms for half way with boost 0.6, so roughly **60 ms later**. Lower the setting to trade smoothness for delay (at 50 the lights are
  half an interval, about 50 ms, behind the newest picture instead of 100). `core.h`: `Motion`, `motionPush`, `motionAt` (the independent version is
  `tools/reference/motion.py`).
* **Blur along the strip** (per target, 0-20 LEDs): each LED along the picture part of the strip is averaged with its neighbours, so the block of one zone
  becomes a soft blob. Three box averages in a row (close to a Gaussian, much cheaper): about 0.3 ms for 266 LEDs per output frame on the AtomS3, plus a
  scratch buffer. `core.h`: `blurColours`.

Both are off by default. Measured on the AtomS3 with a moving test picture (the box played from a PC; two WLED strips of 210 and 266 LEDs plus two Govee
lights with 42 and 50 segments; about 33 output frames a second): the busy core is at 15 % with the old settings, 14 % with motion smoothing (it replaces the exponential smoothing, which
costs more) and 16 % with blur on every target. A still picture costs about 10 % either way, because the output idles at about 13 frames a second and
sends nothing. The longest pass of the main loop stays under 8 ms and the free memory above 170 KB. What they cannot do: the box describes each edge of the picture with 4 or 6 colours, so a moving object stays a blob,
and a Govee segment (3 to 6 LEDs) still changes as one.

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
