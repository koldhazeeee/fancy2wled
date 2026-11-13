# Security

## Read this first: never expose the relay to the internet

Fancy2WLED is built for **a trusted home network**. **Everything it does on the network is in plain text, and the setup page has no password unless you set one** (and even then
it crosses the network in plain text). Do not port-forward it, put it in a router's DMZ, publish it through UPnP, a cloud tunnel or a public reverse proxy, or give it a public address. Anyone who can reach it
can change its settings, and anyone who can watch the traffic can read it.

What travels unencrypted:

| What | Where it is visible |
|---|---|
| The **setup page** (HTTP on port 80). The build with the phone camera calibration also serves HTTPS, but with a self-signed certificate it makes itself, and the normal page stays plain HTTP | everyone on your network |
| The **Wi-Fi password** you type during first setup. The board's own setup Wi-Fi is open (no password) while it waits for you, and the page is plain HTTP | anyone within radio range while you set it up |
| The **firmware update password** and the firmware file itself (over-the-air updates use the same plain HTTP page) | everyone on your network |
| The **MQTT** user name and password for Home Assistant (plain MQTT, no TLS) | everyone on your network |
| The commands to your **WLED** strips (HTTP and DDP) and **Govee** lights (their LAN API over UDP) | everyone on your network |
| The **Sync Box's own broadcast**. Its frame format is public (see [docs/PROTOCOL.md](docs/PROTOCOL.md)) and only obfuscated, not encrypted | everyone on your network |

By default there is no login for the setup page, the same as WLED itself. Since 0.4.13 you can set a **setup page password** (Device tab): every change then has to carry
it (an `X-Fwb-Password` header, or `auth` in the JSON body), compared in constant time, with everything refused for a minute after 5 wrong tries (shared with the update
password); reading the page and the status stays open, and so does the picture map's recount, which changes nothing. It keeps other people and devices on your network
from changing the lights. It is **not** protection against someone who can read your network traffic: it travels in plain HTTP. A config file cannot set it, it is never
sent back or exported, and a factory reset (the button, 8 seconds) clears it. Over-the-air updates are off until you set an update password, and a firmware image has to
be a valid image for this chip that carries this project's marker, but that is a safety check against mistakes, not strong authentication on a plain-text link.

## What to do about it

* **Keep it on a network you trust**, ideally a separate IoT network or VLAN that guests and your less trusted devices cannot reach.
* **Set a setup page password** (Device tab) if other people or devices share your network, and use one you use nowhere else.
* **Lock it to your Sync Box**: on the setup page press **Only accept this box**. Otherwise any device on the network can send the relay colours. (If your router gives the
  box a new address, give the box a fixed one in the router.)
* **Do the first Wi-Fi setup yourself and quickly**: the setup Wi-Fi is open until the board has joined your network, then it disappears.
* **Leave over-the-air updates switched off** when you are not updating, and use a password you use nowhere else (it crosses the network in plain text).
* **Need it from outside the house?** Use a VPN into your home network, then open the setup page through the VPN. Never forward a port to the board.
* Do not share an exported config with the MQTT password ticked, or screenshots and diagnostics with your addresses in them.

## Supported versions

Only the **latest release** gets fixes. Please update first and check the problem is still there: see the [changelog](CHANGELOG.md).

## Reporting a vulnerability

Please **do not open a public issue** for a security problem. Use GitHub's private reporting: on the repository's **Security** tab choose **Report a vulnerability**.
Include what you found, the firmware version (shown on the setup page), what an attacker needs (for example "any device on the same Wi-Fi"), and the steps to reproduce it.

This is a hobby project maintained in spare time, so there is no service level, but reports are taken seriously: expect an answer within about a week, a fix in the next
release when one is possible, and credit in the changelog if you want it.

**In scope:** the firmware, the setup page and its API, the update path, the way settings and secrets are stored and exported.

**Out of scope, because it is how the project works and is documented above:** the lack of a login on a plain HTTP setup page, and the lack of encryption on the local
network. Ideas for improving either are welcome as normal feature requests.

Please never post real addresses, Wi-Fi details or passwords in an issue. The diagnostics file the setup page can download contains no addresses, and the config export
leaves out the Wi-Fi details.
