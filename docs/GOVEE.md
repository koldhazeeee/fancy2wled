# Govee lights as targets

A target can be a **Govee light** instead of a WLED. It follows a part of the picture like any other target, but per segment: the board sends a few colours
and the light stretches them evenly over its whole length. Developed and tested on an H61C3 (RGBIC neon rope, 10 ft).

## Set up

1. In the Govee Home app, switch on **LAN control** for the light (without it the light does not answer on the network).
2. Setup page > *Targets* > **Add a target** > *Type*: **Govee light**. Press **Find** (a scan takes about 3 seconds) or type the light's **IP address**
   (give it a fixed address in your router). **Check this light** shows the model, firmware and whether it is on.
3. Pick **Where the LEDs are** (the edge of the picture the light follows, or all the way around) and the **number of segments** (1 to 60, 20 is a good start;
   the light has its own number of real segments, so going far beyond it gains nothing). Entry 0 is the light's **right-hand end** as seen from the front:
   the page starts with *the numbers go from right to left*; change it if yours is the other way.
4. *Switch the light on while relaying*, its brightness (1-100 %, or -1 to leave it) and *Put the light back how it was when I stop* work like the WLED ones.
   When the sync stops (the box goes quiet, the TV is off with "hand back", you pause it) the light is put back to its earlier on/off state, brightness and
   last plain colour. **A scene** (rainbow, music...) **cannot be restored** over the LAN: the light returns to the last plain colour it had.

## How it works (and what to know)

* Discovery: multicast `239.255.255.250:4001`, the lights answer to port 4002 on the board. Control: UDP to the light on port 4003 (`turn`, `brightness`, `colorwc`,
  `devStatus`). The board only listens on 4002 for a moment while it asks a light something, so it does not clash with other Govee software (govee2mqtt...)
  running on another machine; two programs controlling the same light at once will of course fight.
* Streaming uses the **"razer" mode** (the one DreamView-like programs use): `{"msg":{"cmd":"razer","data":{"pt":"<base64>"}}}` with a binary packet
  `BB 00 01 B1 01 <xor>` to start, `BB <len16> B0 00 <n> <r g b>*n <xor>` per frame, `BB 00 01 B1 00 <xor>` to stop. It is **not part of Govee's documented LAN API**:
  it was checked on one light (5, 10 and 20 entries all covered the whole strip, entry 0 at the right end) and other models may differ. `tools/reference/govee.py` is the
  independent reference the firmware's packets are tested against (`test/golden.txt`).
* The light gets at most about 12 frames a second (UDP, no acknowledgement, no documented limit); unchanged colours are re-sent once a second, and "streaming on" every 10 s.
* UDP gets lost: control commands are sent twice and a state is checked before it is believed (a lone "turn off" was sometimes ignored by the real light).
* The colour of a Govee strip differs from a WLED strip's; the target's brightness trim and the Look settings apply.
* Not available for Govee targets: the LED finder, the markers and the phone calibration (they are for WLED strips).
