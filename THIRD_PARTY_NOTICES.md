# Third-party software

The Fancy2WLED firmware (the `.bin` files of a release) is built from this repository's own code (MIT, see [LICENSE](LICENSE)) and from the software below, which keeps
its own licence. The exact versions are pinned in [platformio.ini](platformio.ini).

| Software | Version | Licence | What for |
|---|---|---|---|
| [ArduinoJson](https://arduinojson.org/) by Benoit Blanchon | 7.4.3 | MIT | the JSON of the setup page's API, the config and the Govee commands |
| [Arduino core for the ESP32](https://github.com/espressif/arduino-esp32) by Espressif Systems and contributors | 2.0.14 (the PlatformIO platform `espressif32@6.5.0`) | LGPL-2.1-or-later | the Arduino environment the firmware runs on: Wi-Fi, mDNS, networking, flash storage |
| [ESP-IDF](https://github.com/espressif/esp-idf) by Espressif Systems | 4.4 (inside the platform above) | Apache-2.0 | the system underneath: FreeRTOS tasks, the HTTP server, mbedTLS, lwIP and the drivers |

ESP-IDF itself carries further components under their own licences (for example lwIP under a BSD licence, FreeRTOS under MIT, mbedTLS under Apache-2.0, newlib under BSD
licences); they are listed with the framework's sources.

**ArduinoJson** is Copyright (c) 2014-2026 Benoit BLANCHON, under the MIT licence: permission to use, copy, modify, merge, publish, distribute, sublicense and sell copies is
granted, provided that the copyright notice and the permission notice are included in all copies or substantial portions of the software. The software is provided "as is",
without warranty of any kind. The full text is in the library's `LICENSE.txt`, which PlatformIO downloads into `.pio/libdeps/` when you build.

**The Arduino core for the ESP32** is licensed under the GNU Lesser General Public Licence 2.1 or later. The firmware is built from source with PlatformIO against the exact
framework version pinned in `platformio.ini`, so anyone can rebuild it, and relink it with a modified version of the framework, from this repository: `pio run -e atoms3`.
The framework's sources and licence texts are in its package (`~/.platformio/packages/framework-arduinoespressif32/` after a build).

**ESP-IDF** is licensed under the Apache Licence 2.0 (https://www.apache.org/licenses/LICENSE-2.0).

## Used when building and testing only (not part of the firmware)

PlatformIO and its ESP32 toolchain (Apache-2.0 and the toolchain's own licences), Python and Node.js, and, for the host tests, a C++ compiler (g++, clang++, or the
[zig](https://ziglang.org/) compiler from the `ziglang` Python package, MIT).

## Protocols

The Sync Box's sync stream was worked out by observing traffic on the author's own network ([docs/PROTOCOL.md](docs/PROTOCOL.md)). The Govee control and "razer" streaming
commands follow what Govee documents for its LAN API and what is publicly described elsewhere; DDP is the open realtime protocol WLED receives. They are re-implemented
here (and checked against an independent Python version in `tools/reference/`), not copied from another project.

## Trademarks

FancyLEDs, Tuya, WLED, Govee, Home Assistant, M5Stack, Espressif and every other product name used here belong to their owners. This project is not affiliated with or endorsed
by any of them.
