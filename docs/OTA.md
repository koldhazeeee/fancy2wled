# Firmware updates over Wi-Fi

After the first USB flash you can update the board without a cable, in two ways. Both are **off until you set an update password and switch updates on**
(setup page, *Firmware updates*), and both need that password.

| Way | How |
|---|---|
| **Setup page** | *Firmware updates*: choose `firmware.bin`, enter the update password, press *Update firmware*. A progress bar shows the upload, then the page waits for the board to come back and shows the new version. |
| **PlatformIO** | Set the environment variable `FANCY2WLED_OTA_PASSWORD` to the update password, then `pio run -e atoms3-ota -t upload` (the board is found as `fancy2wled.local`). |

The file is `firmware.bin` from `.pio/build/atoms3/` (the **app image**, not a merged or bootloader file), or `fancy2wled-atoms3-<version>.bin` from the project's
GitHub *Releases* page (built from the tagged version by the `release` workflow; `SHA256SUMS.txt` has the checksums). The phone-calibration
build has its own file (`.pio/build/atoms3-phonecal/firmware.bin`). Your settings, calibration and MQTT login are kept.

## First time

1. Flash the first firmware that has updates over USB (`pio run -e atoms3 -t upload`). The flash layout already has two firmware slots.
2. Setup page > *Firmware updates* > **Set the password** (8 to 64 characters). Then tick **Allow firmware updates over Wi-Fi**. The tick box is locked until a
   password exists; the board refuses to switch updates on without one.
3. To change the password later you need the current one. A config file you import can never set it (and exports leave it out).

**Forgot the password?** Switching updates off needs no password, but changing the password does. To start over, hold the board's button for 8 seconds (a factory
reset: it clears the Wi-Fi and every setting, the update password included). Flashing over USB keeps the settings, so it keeps the password.

## What protects the board

* Nothing is accepted while updates are off, or without the password. 5 wrong passwords in a row lock every password check for a minute.
* The password travels unencrypted, like the rest of the setup page (it is plain HTTP on your network; PlatformIO's espota uses a challenge, so its password
  is not sent in the clear). Use one you do not use anywhere else. Anyone who can reach the board **and** knows the password can replace the firmware.
* The upload is a raw body with the password in a header (`X-Update-Password`). A page on another web site cannot make your browser send that header, so
  visiting a bad page cannot start an update.
* Before anything is written the file must look like an ESP32-S3 application image (header, chip id, app descriptor), fit the slot, and contain this
  project's marker text: a bootloader, partition table, text file or another project's firmware is refused with a message, and nothing changes.
* The image is written into the slot that is **not** running and only chosen for the next boot when the whole file has arrived and passed the ESP-IDF
  integrity check. A cut connection, a power cut or a truncated file changes nothing (all of these were tested).

## If the new firmware does not work

A new firmware starts as *pending*. If it **crashes or restarts within its first 45 seconds**, the bootloader goes back to the previous firmware by itself and
the setup page says *The last update did not start properly...* After 45 seconds of running it is confirmed (*valid*).

Because the bootloader can only go back to a firmware that was confirmed, **a new update is refused while the running one is still pending** (about the first
minute after any update or restart into new firmware: the page says how many seconds). Without that rule a second update during that minute would leave nothing
to go back to; this was found by testing it (a firmware that crashed at boot then restarted forever until flashed over USB).

If a firmware starts fine but is bad in a way that does not crash it (for example it cannot join your Wi-Fi), the board cannot know: flash over USB.

## Details

* While an update is written the WLEDs are handed back, nothing is streamed and the board's LED blinks magenta; Home Assistant's *State* shows
  *updating firmware*. The board restarts by itself afterwards.
* `GET /api/ota` returns `{enabled, passSet, version, slot, imageState, maxBytes, rolledBack, busy}` (no secrets). `POST /api/ota` is the upload.
* The check of the file is `checkAppImage` / `MarkScanner` in `include/core.h` (tested in `test/test_core.cpp`).
* The slots are `app0` and `app1`, 3264 KB each (`default_8MB.csv`); the firmware is about 1.1 MB.
