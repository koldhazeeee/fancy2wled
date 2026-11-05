# Changelog

What changed in each release, written for someone deciding whether to update. Updating over Wi-Fi keeps your settings (new settings start at a safe
default). Anything you may want to check after updating is marked **After updating**. The full list of commits is on the
repository's releases page.

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
