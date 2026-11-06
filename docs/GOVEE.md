# Govee lights as targets

A target can be a **Govee light** instead of a WLED. It follows a part of the picture like any other target, but per segment: the board sends a few colours
and the light stretches them evenly over its whole length. Developed on an H61C3 (RGBIC neon rope, 10 ft); see *Tested models* below.

## Tested models

**Check this light** (setup page) asks the light for its model. For the models below the page fills in the number of segments (when it is still the
starting 20; otherwise it offers a button), and says so. For any other model it says the number is a guess.

| Model | Light | Segments | How it was measured |
|---|---|---|---|
| H61C3 | RGBIC neon rope, 10 ft | 42 | alternating red / blue with 42 entries gave 21 red and 21 blue blocks, each about 2.9 in |
| H61B5 | RGBIC strip with covers, 16.4 ft | 50 | alternating red / blue with 50 entries gave equal blocks of 6 LEDs each (300 LEDs at 60 per metre = 50 blocks) |

The number cannot be read from the light (the LAN API does not report it, and the Govee app's own "50 segments" for DIY mode need not match it), so it
is measured: light the strip with alternating colours and count. To add a model, stream N entries (60 is the most the board sends) alternating red and blue
(the packets are described under *How it works* below), find the N for which the blocks come out equal in width with every one of them showing, then add the
model to `GOVEE_MODELS` in `web/index.html` together with a row in this table.

## Set up

1. In the Govee Home app, switch on **LAN control** for the light (without it the light does not answer on the network).
2. Setup page > *Lights* > **Add target** > *Connection* > *Type*: **Govee light**. Press **Find** (a scan takes about 3 seconds) or type the light's **IP address**
   (give it a fixed address in your router). **Check this light** shows the model, firmware and whether it is on.
3. Pick **Where the LEDs are** (the edge of the picture the light follows, or all the way around) and the **number of segments** (1 to 60, 20 is a good start;
   the light has its own number of real segments, so going far beyond it gains nothing; **an H61C3 has 42**, each about 2.9 in on a full 10 ft strip, and
   all 42 can be driven separately: alternating 42 colours showed 21 red and 21 blue blocks on a real one). Entry 0 is the light's **right-hand end** as seen from the front:
   the page starts with *the numbers go from right to left*; change it if yours is the other way.
   **Only part of the strip along the picture?** A Govee light always stretches the colours over its whole length, so the number of segments is not a
   length. If only part of the strip runs along the screen's edge (say 100 in of a 16.4 ft strip), give the **first and last segment along the picture**
   (section 3). The picture is then spread over just those, and the segments outside it hold the colour of the zone at that end, or stay **dark** if you
   tick *Keep the segments outside it dark* (use that when the rest of the strip is coiled up or hidden). For a tested model the page also says how many
   inches that is.
   **Find my segments** lights the segments you choose in white, one at a time by default, so you can see exactly where a segment starts and ends and which
   ones lie along the picture; *Show every segment, alternately red and blue* lights all of them in turn red and blue (segment 0 is red), the same pattern
   used to count the segments of a new model. The light is switched on for it and put back when you press **Done** (or close the page).
4. *Switch the light on while relaying*, its brightness (1-100 %, or -1 to leave it) and *Put the light back how it was when I stop* work like the WLED ones.
   When the sync stops (the box goes quiet, the TV is off with "hand back", you pause it) the light is put back to its earlier on/off state, brightness and
   last plain colour (a light that was off comes back off: the first frame is only sent once the light's state has been read, because it switches the light on). **A scene** (rainbow, music...) **cannot be restored** over the LAN: the light returns to the last plain colour it had.
   If the board restarts while it is relaying (a firmware update, a crash, a power cut) it cannot put the light back at that moment. It keeps the
   light's earlier state in its flash memory for as long as it relays, and puts the light back from that record the next time it starts, so the
   light is not left on, and the next sync does not mistake the relaying state for the light's own.

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
* The colour of a Govee strip differs from a WLED strip's; the target's brightness trim and the Look settings apply. A Govee target starts with **sharp zone edges** (each of the Sync
  Box's zones gets its own block of segments, 7 each for 42 segments on the bottom edge) because a smooth blend between unlike colours passes through muddy grey and brown on a light you
  look at directly; slide *Zone edges* towards smooth if you prefer soft gradients. **Vivid blending** (on by default for a Govee) keeps the colours between two zones vivid
  instead of mixing them in RGB: with smooth edges every one of the 42 segments of an H61C3 shows its own colour, the zones being joined through the colour wheel (red, orange, yellow, green), not through grey. A diffused neon rope also looks pale: raise the target's **colour saturation** (130 % is a good start).
* Not available for Govee targets: the LED finder, the markers and the phone calibration (they are for WLED strips).
