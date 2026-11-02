# If a Sync Box firmware update breaks Fancy2WLED

Fancy2WLED reads what the box broadcasts; it was reverse-engineered from one box (FSB3P2.1, firmware Main 3.1.0 / MCU 2.0.14 / Other 1.1.3), and
the maker can change that broadcast in any update. This page explains how you will notice, what to do, and how to be ready.

## How you will notice

Before this existed a changed broadcast looked exactly like a box that is switched off ("waiting for the Sync Box"). Now the relay sorts every datagram
that arrives on the Sync Box port into three kinds:

| Kind | Meaning |
|---|---|
| **understood** | decodes: the colours are used |
| **Sync Box framing, payload not understood** | the datagram starts like a Sync Box frame (the magic bytes, and the length byte matching the size) but the payload is not readable: the box's firmware probably changed the payload (a new marker, more zones, a new key scheme) |
| **unreadable** | anything else (other data on that port, a changed header, a damaged datagram) |

When datagrams arrive but none can be read for a few seconds you get:

* the **setup page** shows a red *Sync Box: frames not understood* warning with the counts and a **Download diagnostics** button;
* the **board's LED** blinks red;
* **Home Assistant** gets the state *Sync Box frames not understood*, a *Sync Box frames not understood* problem sensor and a *Frames not understood* counter.

While it is only silence (nothing arrives at all) it still says *waiting for the Sync Box*: the box is off, or not on this network, or it stopped sending.

## What to do

1. Press **Download diagnostics** and attach `fancy2wled-diagnostics.json` to an issue on the project's GitHub page, with your box's firmware versions
   (from its app). The file holds the counters and a few raw frames (colours and sizes only: no addresses). Nothing is sent anywhere by the relay.
2. `python tools/check_capture.py fancy2wled-diagnostics.json` reads that file and says what changed, for example *"sizes [112], payload starts with ..., a NEW
   PAYLOAD LAYOUT, not corruption"*.
3. If you kept a capture from before the update, record a new one and compare (below): that shows exactly which part of the protocol moved.

## Be ready before an update

Record a reference while everything works, and keep it:

```
python tools/capture_frames.py --seconds 240 --label before-update --box-firmware "main 3.1.0 / mcu 2.0.14 / other 1.1.3" --note "what was on screen"
python tools/check_capture.py captures/box-....jsonl --save captures/before.json
```

Captures go to `captures/` (git ignores it). They contain your network addresses, so never commit or post them; back the folder up, because it is the
only record of how your box behaved before. Several captures (a bright scene, a dark scene, a different input, standby) make the comparison stronger.

After an update, record again with `--label after-update` and run

```
python tools/check_capture.py captures/box-....jsonl --compare captures/before.json
```

It lists every difference that matters: datagram sizes, magic bytes, header bytes, the checksum, the key lengths, the payload marker and size, the number of
zones, the source port, the frame rate, and how strongly the box smooths its own colours (the *Reaction boost* assumes 0.6 per step; if it changes, adjust
`kBoxLead` in `include/core.h`).

## What the relay already tolerates

| If the update changes... | Result |
|---|---|
| header bytes 4-7 (including the checksum) | nothing: they are not used |
| the box's frame rate, the colours' curve | nothing: only the colours matter (a changed smoothing makes the Reaction boost over- or under-shoot) |
| the magic bytes, the length byte, the size range 86-93, the payload marker `06 04 06`, the number of zones, the key scheme | the relay cannot read the frames: it reports **frames not understood** (above) and the lights go back to WLED's own state |
| the UDP port, or broadcast to unicast / multicast | nothing arrives: it looks like a silent box |

## Stopping surprise updates

Whether a Tuya device updates on its own, or only when you accept it, is decided by the device's maker, and Tuya's app checks for updates when you open the
device's panel. I found no public changelog for the FancyLEDs box and could not confirm a per-device switch that turns updates off. The one measure that
works regardless is at your router: block the box's access to the internet (a firewall rule for its address). The sync broadcast is local, so it should keep
working, but the Smart Life app's remote control and cloud features would stop, you would never get the maker's fixes, and this has not been tested with this
box. It is reversible: delete the rule to let the box update.

## For developers

* `classifyFrame()` in `src/core.cpp` (mirrored by `classify_frame()` in `tools/reference/protocol.py`, tied together by the `CLASSIFY` vectors in
  `test/golden.txt`) is the single place that decides understood / new layout / unreadable.
* Header byte 5 is a checksum (167 + the sum of every byte except bytes 3 and 5, modulo 256). The decoder ignores it on purpose; it is only reported, so a
  changed checksum can never break the relay, while a valid checksum on an unreadable frame says "same protocol, new payload".
* The relay keeps the last four unreadable datagrams (at most one every half second) and one understood frame; `GET /api/diag` returns them.
* `python test/test_capture_tools.py` covers the capture and comparison tools.
