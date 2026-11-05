## What this changes, and why

<!-- One change per pull request. The reason matters more than the diff. -->

## Checklist

- [ ] Tests come with the change (core: `test/test_core.cpp` and, for a calculation, the independent Python version in `tools/reference/`; setup page: `test/test_pages.js`)
- [ ] `node test/test_lint.js` passes: sources are ASCII with LF endings and hold **no real IP or MAC addresses** (use `192.0.2.x`)
- [ ] The tests in CONTRIBUTING.md pass and both firmwares build (`pio run -e atoms3` and `pio run -e atoms3-phonecal`)
- [ ] A line in `CHANGELOG.md` under *Unreleased*, if a user would notice the change
- [ ] Docs updated (README, or the file under `docs/`) where behaviour changed
- [ ] Nothing private is in the diff, the commit messages, screenshots or attachments (addresses, Wi-Fi details, passwords, captures)
- [ ] If this affects timing or size, I say how I measured it
