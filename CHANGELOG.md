# Changelog

Each version's notes are what the bridge shows before installing it, so keep them under 500 bytes.

## 2.2.0

Release candidates: a bridge running an RC (from the GitHub pre-releases) is offered the final release when it comes out. Bridges set up before the rename move their update source to the new address by themselves. The CH340/CH341 and FTDI baud-rate code was rewritten: same values at every standard rate, and FTDI sticks no longer get divisors the chip doesn't have (above 1.5 Mbaud, below 184 baud).

## 2.1.3

Security fixes (advisory GHSA-g7fj-vfx8-3h8f): every change now needs the dashboard's own request header, also when a browser sends a saved password, so another website can't change settings or install firmware; /metrics no longer makes browsers save the password; a download could write to freed memory. Please update.

## 2.1.2

A quieter log: the web server closed each idle browser connection by cutting it off, and every cut logged "httpd_sock_err: error in recv : 128" (a few per dashboard visit). Idle connections now get a normal close first; only a client still holding on 5 s later is cut off, and only that one is logged.

## 2.1.1

Updates from GitHub work every time: the bridge rebuilt each redirect without its query string, which drops the signature of GitHub's signed download address, so the download server sometimes answered "HTTP 618". Redirects now follow the server's address exactly. A bridge on 2.1.0 or earlier that shows HTTP 618: check again, or install this release's .bin on System, Firmware.

## 2.1.0

Security and robustness pass. Fixed a heap write from a crafted TI .hex, zip bombs and slow clients that froze the web server. The API token can't install firmware; logins lock out per address; first-boot setup only from the bridge's own address. Update sources must be https. A stuck stick no longer restarts the bridge. Thread RCP .gbl files flash on Thread radios. CI runs the tests on every push.

## 2.0.2

Updates, firmware checks and the stick list try once more when GitHub's download server answers with a server error (it sometimes does for a few minutes after a release is published), and the server's message goes to the log.

## 2.0.1

More free memory: the radio tasks' stacks and the network buffers moved to PSRAM and the other stacks were trimmed to what they use (about 50 KB more internal RAM free than 2.0.0). System shows every task's stack and the heaps. The header shows one pill per radio, named after its app.

## 2.0.0

Up to four radios behind a USB hub, set up by themselves: plug a stick in and it gets a radio, role and port, live. Unplug one nothing used and it's gone; a radio in use waits for its stick. Settings apply without a reboot. ZHA role, per-radio firmware checks, Overview, Devices and Events per radio or all together, SD card formatting, and "stick" everywhere.

## 1.0.26

Settings shows every radio the same way: on/off, name, role, TCP port, baud rate, allowed host and the client setup, radio 1 included (the separate Zigbee2MQTT and Dongle role cards are gone). A role that doesn't fit the stick's firmware is flagged. Events use a radio's new name straight away.

## 1.0.25

New radio role for radios 2 and 3: Zigbee coordinator for Home Assistant's ZHA, so Zigbee2MQTT and ZHA can each run their own network through one bridge. Settings, Radios shows the ZHA setup (radio type, socket:// path, port speed). Radio 1 stays the Zigbee2MQTT radio.

## 1.0.24

Zigbee network status shows "up" again with EmberZNet 8 and later (EZSP v14+), which report it with the newer status codes. Before, the bridge showed the network as unknown even when it was up.

## 1.0.23

Up to three radios behind a powered USB hub on J3, each with its own TCP port: Zigbee (6638), Thread RCP for the OpenThread Border Router (6639) and Z-Wave for Z-Wave JS (6640). J3 now runs at full speed so hubs work with the sticks. Turn radios on in Settings, Radios; System shows the USB devices. One dongle on J3 works as before.

## 1.0.22

First public release. New board: flash zigbee_eth_bridge-1.0.22-full.bin at 0x0 with esptool (esptool --chip esp32p4 write_flash 0x0 zigbee_eth_bridge-1.0.22-full.bin), then open http://zigbee-eth-bridge.local and create a password. Later updates install from the dashboard (System, Updates).
