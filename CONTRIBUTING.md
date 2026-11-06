# Contributing

Thanks for wanting to help. Bug reports with a diagnostics file, reports that your Sync Box model or Govee light works (or does not), and small focused pull requests
are all welcome.

## Before you open an issue

* **Security problem?** Do not open an issue: see [SECURITY.md](SECURITY.md).
* **Sync Box not working after an update of its firmware?** Download the **diagnostics** file from the setup page and attach it. It holds counters and a few raw frames
  (colours and sizes only, no addresses). [docs/RESILIENCE.md](docs/RESILIENCE.md) explains how it is read.
* Say which **version** you run (shown on the setup page), your **Sync Box** model and firmware version (from its app), the board, and the kind of target (WLED
  version, or the Govee model).
* **Never post** real IP or MAC addresses, Wi-Fi names or passwords, MQTT passwords or an exported config with the MQTT password ticked. Replace addresses with
  `192.0.2.x`.

## Building and testing

You need Python 3.12, Node 24 and [PlatformIO](https://platformio.org/). Details are in the README under *Building and testing*; the short version:

```
python test/run_core_tests.py     # the C++ core against golden vectors from an independent Python version, plus fuzzing under sanitizers
node test/test_pages.js           # the real setup page in headless Edge/Chrome against test/mock_device.js
node test/test_analysis.js        # calibration maths on simulated camera frames
node test/test_lint.js            # sources are plain ASCII with LF line endings, and hold no real device addresses
python test/test_check_size.py && python test/test_gzip_web.py && python test/test_capture_tools.py
pio run -e atoms3 && pio run -e atoms3-phonecal
```

CI runs the same checks on every push and pull request, and a release is only published when they all pass.

## Rules for changes

* **Tests come with the change.** A change to the core (`src/core.cpp`) needs a check in `test/test_core.cpp`, and where it is a calculation the independent Python
  version in `tools/reference/` has to agree (the golden vectors are generated from it). A change to the setup page needs checks in `test/test_pages.js`.
* **No real addresses anywhere** (code, tests, docs, screenshots, commit messages): use `192.0.2.x`, or the examples listed in `test/test_lint.js`. The lint fails
  on anything else, so it is the first check to run. To run it before every commit install the hook once:
  `cp tools/git-hooks/pre-commit .git/hooks/pre-commit` (git does not version hooks).
* **Sources are plain ASCII with LF line endings.** The board shows non-ASCII garbled.
* **Add a line to [CHANGELOG.md](CHANGELOG.md)** under *Unreleased* for anything a user would notice, written for someone deciding whether to update.
* **Measure, do not guess.** Timing, size and CPU claims in the docs come from measurements (see [docs/LATENCY.md](docs/LATENCY.md), `tools/sync_probe` and
  [docs/PERFORMANCE.md](docs/PERFORMANCE.md)); say how you measured when you change one. A speed-up is measured on the board (the PC can disagree) and must not change what the
  relay outputs: `tools/freeze_legacy.py` keeps the original functions for `test/test_core.cpp` to compare against, and `tools/output_fingerprint.py` compares complete LED output.
* Keep the firmware small and the loop fast: CI fails an image above 90 % of its update slot, and the output loop runs about 30 times a second.
* Do not add dependencies lightly; the build is pinned (PlatformIO, the ESP32 platform, ArduinoJson) on purpose.
* Keep pull requests focused: one change, with the reason in the description.

## Hardware you can help with

The relay is developed on an **M5Stack AtomS3 Lite** against a **FancyLEDs "3 Port 2.1 Fancy Sync Box" (FSB3P2.1)**, WLED strips, and Govee H61C3 and H61B5 lights. Reports
from other boxes, boards, WLED versions or Govee models are the most useful thing you can add. For a new Govee model, [docs/GOVEE.md](docs/GOVEE.md) describes how its
segment count is measured. For a Sync Box that behaves differently, `tools/capture_frames.py` and `tools/sync_probe` record what it sends. The recordings stay on your
computer (they are git-ignored); the ones from `capture_frames.py` contain your network's addresses, so do not attach them: attach the diagnostics file instead.

## Licence

By contributing you agree that your contribution is licensed under the [MIT licence](LICENSE) of the project.
