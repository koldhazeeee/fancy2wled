# Changelog

What changed in each release, written for someone deciding whether to update. Updating over Wi-Fi keeps your settings (new settings start at a safe
default). Anything you may want to check after updating is marked **After updating**. The full list of commits is on the
repository's releases page.

## Unreleased

* **Faster output, same lights.** The colour work of an output tick (mixing the LEDs from the zones, blur, saturation, easing, the Dark scenes stage) is about 1.8 times faster on
  the board, measured on the real ESP32-S3 and on the author's real three targets (392 to 216 microseconds per tick; 473 to 247 with Smoothing over time at 0.5). While a target
  eases toward a picture that has not changed it no longer recomputes the LEDs (38 to 52 % of ticks on the real setup). Reaction boost works its factor out once per packet. The LED
  output is byte for byte the same as before (checked on the board with 36 fixed pictures, and by comparing every optimised function with its original on random input). It costs
  about 6 KB of flash and 1.2 KB of RAM. Dead code removed. The method, the benchmarks you can run yourself and every number are in [docs/PERFORMANCE.md](docs/PERFORMANCE.md).

## 0.4.10 - 2026-11-05

* **Dark scenes and flicker** (setup page, Look; all off by default, so nothing changes until you use them). A last step on the colours, after Reaction boost, smoothing and
  Motion smoothing so it does not fight them:
  **Black cut-off** (an LED whose brightest colour is below N levels goes completely dark, so dark noise stays black; one level of hysteresis so it does not flicker at the edge),
  **Hold steady** (a colour keeps its value until the picture has moved it by more than N levels, which stops a steady scene flickering by one level; a fade to black still
  reaches black) and **Dither** (while the colours move, the rounding error is carried to the next frame so slow fades and dark scenes show the steps between whole levels).
  On the real board, with noise in a dark picture: 84 flickering changes without it, 1 with a cut-off of 8. The Settings check points out Dither together with a Hold steady
  of 1 or more, where it does nothing. Checked against an independent Python version on 30 simulated sequences, under the sanitizers, and on the board.

## 0.4.9 - 2026-11-05

* **The relay measures the Sync Box's Speed of Diffusion** from the picture changes it sees (scene cuts, flashes): after a sudden change the box's second step is (1 - speed)
  times its first. The latest 24 such measurements vote for their speed (which copes with real video that keeps moving after a cut), and the answer shown is held steady against flicker. The setup page shows the result under
  *Box speed* ("Measured from the Sync Box: about 75 %"), and the *Settings check* warns when your *Box speed* is 10 % or more away from it, so Reaction boost does not undo the
  wrong amount of smoothing. **Advice only**: nothing the relay does with the colours changes. It says no value stands out yet until the vote is clear, and cannot measure below
  about 20 %. Checked on recordings of the real box (40 %, 75 % with a TV programme, 100 %) and on simulated boxes with lost packets; see [docs/LATENCY.md](docs/LATENCY.md). The
  status has the numbers as `box.speed`.
* **Repository files for going public:** SECURITY.md (never expose the relay to the internet: everything is plain text and the setup page has no password), CONTRIBUTING.md,
  issue forms and a pull request checklist, THIRD_PARTY_NOTICES.md, a tested-with list in the README, read-only permissions for CI and Dependabot for the GitHub Actions.

## 0.4.8 - 2026-11-05

* **Fix: a Govee light that was off before sync came back on after a pause.** The relay read the light's state in the background while its first colour frame
  (which also switches streaming on) was already on its way, so the light was already on when its state was read and was handed back on. A Govee light now gets no frames
  until its state has been read. A light that was on still comes back on at its own brightness. Found by reproducing it on a real light and logging what the board read.

## 0.4.7 - 2026-11-05

* **Only accept this box** (setup page, status card): locks the relay to your Sync Box's address, so no other device on the network can send it colours. If the box gets a new
  address from your router the page says frames from another address are being ignored and offers to follow it. An address that is not an IP address (a name, `0.0.0.0`, a typo)
  is refused, because it would silently switch the lock off. **After updating:** nothing changes until you press the button.
* **Last restart**: the setup page (Device) and the diagnostics file say why the board last restarted: power, a restart, a crash, a watchdog or a brownout. A crash, watchdog or
  brownout is called out in the status line.
* Behind the scenes: releases are published only after every test passes and need a changelog section for the version, whose text becomes the top of the release page;
  the firmware size is checked in CI (fails above 90 % of the update slot; now 34 % and 35 %); the parsers that read network input are fuzzed under sanitizers; the build is pinned.

## 0.4.6 - 2026-11-04

* **Settings check**: a card on the setup page that looks at your settings (saved or not) and says what is wrong (**Fix**) or could be better (**Tip**): a Reaction
  boost that does nothing, settings that Motion smoothing overrides, a box left at its slow default speed, a TV part that is missing or outside the run, two targets
  driving the same LEDs of one WLED.

## 0.4.5 - 2026-11-04

* **Click a zone on the picture map to light the LEDs that follow it** on your WLED strips (white, 8 seconds, click again to switch it off). Govee lights are not
  included.
* README: a new section, "What the Sync Box sends", with what was measured about the box (see 0.4.4).

## 0.4.4 - 2026-11-04

* **The zones are now placed where the Sync Box really looks.** It does not cut the picture into an even 6 x 4 grid: each zone is an exact average over one window
  (thin bands along the top and bottom, a gap in the middle, two tall windows per middle row, 60 px strips at the sides). The top and bottom edges moved by about 2 %;
  the **left and right edges changed more** (the corner zones are at 3 % and 97 % of the height, not 12 % and 87 %). The picture map draws the real windows.
  **After updating:** LEDs near the side corners of a left or right strip will look a little different. That is the fix.
* **Box speed** (Look): set it to the number in the Sync Box app's *Speed of Diffusion* (default 40). **At 100 % the box does not smooth at all**, which is the best
  setting for a low delay. *Reaction boost* now undoes the box's smoothing exactly at any speed, and copes with packets Wi-Fi dropped. **After updating:** if you changed the
  box's speed, enter the same number here; a boost does nothing at a box speed of 100.
* `tools/sync_probe`: shows known pictures to a box and reports its delay, smoothing, levels and the window each zone looks at (flicker tests can be left out).

## 0.4.3 - 2026-11-04

* **Motion smoothing** (Look, off by default): slides the lights from one Sync Box picture to the next instead of jumping, so movement is about 2.7 times smoother. It adds
  delay (about 60 ms later than a Reaction boost of 60 %), and replaces *Smoothing over time* and *Reaction boost* while it is on.
* **Blur along the strip** (per target, off by default): softens the steps between zones.
* **Delay to the lights**: a panel in the status card shows, for each target, the time spent on the board, the network and the strip; `tools/latency_e2e.py` measures the
  whole delay from the box to a WLED strip.
* Faster: a still picture costs less CPU, Govee lights skip colour work between their frames, and the vivid blend is cheaper.

## 0.4.2 - 2026-11-03

* **Govee**: known models fill in their segment count (H61C3 and H61B5), a "part of the strip in use" with an option to keep the rest dark, and a **segment finder** to see
  where a segment starts and ends.
* The setup page loads about three times faster (compressed, cached, no waiting on small replies).
## 0.4.1 - 2026-11-03

* A button that lights the **middle LED of the TV part**, to check the strip is centred on the TV.
* Fix: after the board restarted while streaming to a Govee light (update, crash, power cut), the light was left on at the stream's brightness. It is now handed back.

## 0.4.0 - 2026-11-03

* **Better colours on a long strip** (per target): *zone edges* (smooth blend or one hard block of LEDs per zone), *vivid blending* (blend along the colour wheel so red and
  green meet in bright yellow, not muddy brown) and *saturation*.
* The picture map shows the **live colours** the box is sending.

## 0.3.1 - 2026-11-03

* A calmer, minimal setup page that works on a phone and a desktop, with a light and a dark theme.
* The picture map counts a Govee light in segments.

## 0.3.0 - 2026-11-02

* **Govee lights as targets**: followed per segment over the LAN, with the same layouts and picture map as a WLED.

## 0.2.0 - 2026-11-02

* **Firmware updates over Wi-Fi**: upload on the setup page or push with PlatformIO. Off until you set an update password; a firmware that fails to start is undone by
  the board itself.
* **Follows the TV's power**: when the picture stays black the WLEDs are handed back, or switched off, and return with the first colour.
* A WLED can be given by **name**; the board looks it up again every minute, so a new address from your router is found again.
* A **picture map** that shows which zones each target follows; multiple targets; Home Assistant lights for each target; configuration export and import.
* The relay tells you when a Sync Box firmware update changes what it sends (setup page, LED, Home Assistant) and can give you a diagnostics file.
* Faster: frames are sent the moment they arrive, plus an optional **Reaction boost**.
* GitHub releases are built from version tags.

## Before 0.2.0

The first state of the project: an ESP32-S3 (AtomS3 Lite) that listens to the Sync Box's broadcast and streams the colours to a WLED over DDP, with Wi-Fi setup and a
setup page, and an optional build with auto-calibration from a phone's camera.
