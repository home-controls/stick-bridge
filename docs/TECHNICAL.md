# Zigbee Bridge: technical reference

Puts Zigbee2MQTT's coordinator dongle on the network so it can sit in the best radio spot,
away from the Home Assistant box. Custom ESP-IDF firmware with its own dashboard.

One of three Zigbee apps with one look and one logo: **Zigbee Studio** ([zigbee-studio](https://github.com/home-controls/zigbee-studio), the
Zigbee2MQTT UI add-on, which also shows this bridge), **Zigbee Sniffer** ([zigbee-sniffer](https://github.com/home-controls/zigbee-sniffer)) and this
one. The dashboard is built on the shared **Zigbee UI** kit ([zigbee-ui](https://github.com/home-controls/zigbee-ui)), copied into
`web/src/kit/`; don't edit it here (`npm run build` runs `node src/kit/check.mjs`, which fails on a
hand-edited copy): change the kit, then `node <zigbee-ui>/tools/sync.mjs web/src/kit`. The firmware
project and image are called `zigbee_eth_bridge`.

```
Sonoff Dongle Plus MG24 ─USB─▶ ESP32-P4 (J3, USB host) ─TCP 6638─▶ Zigbee2MQTT
                                     │
                                     └─ port 80: dashboard + JSON API (Zigbee Studio, Prometheus)
```

Out of the box the bridge takes an address by DHCP (hostname `zigbee-eth-bridge`, also on mDNS as
`zigbee-eth-bridge.local`) and lets any host connect; on its page set a static IP and the one host
Zigbee2MQTT runs on. Zigbee2MQTT then uses (with your bridge's address):
`serial: { port: tcp://192.168.1.50:6638, adapter: ember, baudrate: 115200, rtscts: false }`.

## Hardware

| Part | Notes |
| --- | --- |
| J3 (4-pin MX1.25) | `VCC_5V / USBD_N / USBD_P / GND`: the P4's USB 2.0 HS host with 5 V. **The dongle goes here** (MX1.25-4P to USB-A female pigtail; check pin order). |
| Type-C | CH343P USB-UART to UART0 (console, flashing) + 5 V in. Device-only: it can't host the dongle. |
| Ethernet | Internal EMAC + IP101 PHY (MDC 31, MDIO 52, reset 51, RMII clock in on 50, TX_EN 49, TXD 34/35, CRS_DV 28, RXD 29/30). PoE module on board. |
| BOOT button | GPIO35 is also Ethernet **TXD1**: firmware must never touch it after boot. |
| Chip | ESP32-P4 **rev 1.3** (needs `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`). 32 MB flash; the image is built for 16 MB and the stored log uses the upper part (see [Stored log](#stored-log)). |
| microSD slot | SDMMC 4-bit: CLK 43, CMD 44, D0-D3 39-42; card power through a P-MOSFET on GPIO45 (low = on); the pins' I/O bank (VDDPST_5) is fed by the P4's LDO 4. No card-detect pin. (ESP32-P4-ETH schematic.) |

## What the firmware does

- Bridges bytes between one TCP client (the allowed Z2M host) and the dongle's CP2102N, set up the way
  Linux sets it up for Z2M (8N1, no flow control, DTR+RTS on). Replaces a stale connection from the same
  host, TCP keepalive, drops Z2M when the dongle goes away.
- Decodes ASH + EZSP (v14+) passively in both directions: link errors (CRC/NAK/retransmit), EZSP command
  round trip, messages in, deliveries ok/failed, per-device last-hop RSSI/LQI and delivery, route errors,
  joins/leaves, network up/down, coordinator firmware version. Layouts checked against zigbee-herdsman.
- Dashboard (Preact + signals on the Zigbee UI kit): Overview, Devices,
  Events, Settings, System (firmware upload, log storage, live + stored log).
- **First boot:** no password exists; the page asks for one. **Factory reset:** power the board on
  5 times in a row, each time for under 10 s (PoE: unplug and replug), or System → Factory reset.
- Settings in NVS: static IP or DHCP, hostname, allowed Z2M host (IP/CIDR, live), TCP port, baud (reboot).
- Firmware updates from the page: image checked (chip, project name), boots on trial, kept after 30 s,
  otherwise the bootloader rolls back.
- **Stored log**: every log line with its UTC time, kept in flash through reboots and power cuts (months of
  it), plus one file per day on a microSD card when one is in (see [Stored log](#stored-log)).
- **The dongle's own firmware** (EmberZNet `.gbl`): uploaded to the bridge, checked, kept, then flashed to the
  dongle on request (see [Flashing the dongle](#flashing-the-dongle)).

## API (for Zigbee Studio and scripts)

Auth: browser session cookie (+ `X-Bridge: 1` on changes), `Authorization: Bearer <token>` (create it under
Settings → API access; shown once), or Basic `admin:<password>`. The token can read everything and change
settings, not the password or the token. mDNS: `zigbee-eth-bridge.local`, service `_zbbridge._tcp`
(TXT `api=zbbridge/1`, `zigbee_port`, `version`).

| Method | Path | |
| --- | --- | --- |
| GET | `/api/state` | no auth: setup_required, authed, version, hostname, api_token_set |
| POST | `/api/setup` `{password}` | first boot only |
| POST | `/api/login` `{password}` / `/api/logout` | session cookie |
| GET | `/api/status` | live state, health, counters |
| GET | `/api/history` | 15 min per second, columns `t in out rx ok fail cmds rtt rttmax err` |
| GET | `/api/events?since=<id>` | event log |
| GET | `/api/devices` | devices heard (short, ieee, rx, rssi/lqi, tx_ok/fail, route_err, last seen) |
| GET | `/api/log?since=<pos>` | ESP log text, this boot (32 KB in memory) |
| GET | `/api/logs` | stored log: `flash` (where, size, used, oldest stamp in Unix ms, boot number) and `sd` (state, card, space, newest day files) |
| GET | `/api/logs/text?src=flash[&tail=<bytes>]` / `?src=sd&name=<file>` | stored log as text/plain, oldest first; `&download=1` saves it as a file |
| POST | `/api/logs/clear` | session only; hides the stored flash log (card files stay) |
| GET / POST | `/api/config` | settings; POST answers with `reboot_required`. `tx_chunk` (0 = unpaced, 8-64), `tx_gap_us` (0-5000), `xoff_honor`, `xoff_max_ms` (1-200) apply at once. 1.0.23: `role` also `zwave`; 1.0.25: radios 2-3 also `zha` (refused on radio 1); `usb_fs_only`; `radios` `[{index, enabled, name, tcp_port, allow, baud, role}]` (radio 0's are the flat fields; either may be posted) |
| POST | `/api/reboot`, `/api/stats/reset`, `/api/ota` (raw .bin body) | |
| POST | `/api/password`, `/api/token`, `/api/token/revoke`, `/api/factory-reset {confirm:"RESET"}` | session only |
| GET | `/api/update` | updates from git: state, current, latest, notes, size, done, md5, sha256, error |
| POST | `/api/update/check`, `/api/update/install` | install answers 409 with the reason when there's nothing to install |
| GET | `/api/dongle[?radio=n]` | the dongle's firmware: what it runs, the stored image (name, size, MD5, SHA-256, metadata, `problem`), flash progress/result. `?radio=n` (1.0.23) picks the radio these pages work on (it stays picked; 409 while another radio's stick is being flashed); the flash/check/download POSTs take it too |
| GET | `/api/usb` | 1.0.23: USB devices on J3 (address, hub port, hub, speed, USB ID, product, radio), `fs_only`, `fs_forced`, `reforces`, `refused` |
| POST | `/api/radio/forget` `{radio}` | 1.0.23, session only: free a radio's saved stick (409 while a stick is plugged into it) |
| POST | `/api/dongle/image?name=<file>` (raw .gbl body), `/api/dongle/image/delete` | session only; the upload answers with the new state or 400 + why |
| POST | `/api/dongle/flash` `{md5}` | session only; `md5` must be the stored image's; 409 + why when it can't start |
| GET | `/api/dongles` | the dongles the bridge recognises (USB ID, chip, radio, protocol, bootloader entry, flashable) |
| POST | `/api/dongle/firmware/check`, `/api/dongle/firmware/download` `{name}` | session only; GitHub firmware check / download into the image area; state in `GET /api/dongle` → `catalog` |
| GET | `/metrics` | Prometheus (Basic auth is checked with PBKDF2 once, then the header is remembered for 10 min until the password changes); 1.0.23 adds `zbbridge_radio_*{radio="1".."3"}` (stick/client connected, bytes each way, client drops) |

`/api/status` keeps every field it had (radio 0 = the dongle) and adds `radios`: one entry per radio with its link
(`enabled`, `name`, `role`, `tcp_port`, `allowed`, `baud`), `stick` (present, open, USB ID, serial, model, protocol,
`fw_running`, `hub_port`), `saved_stick`, `client` and `counters` (bytes and bytes/s each way, sessions, drops). The mDNS
TXT record keeps `zigbee_port` and adds `ports` (every enabled radio's port, radio 1 first).

## Build and flash

No toolchain needed for a first flash: each [release](https://github.com/home-controls/zigbee-bridge/releases/latest)
has `zigbee_eth_bridge-<version>-full.bin` (bootloader, partition table and app in one file). With the board on
its Type-C port:

```
pip install esptool
esptool --chip esp32p4 -p COM3 write_flash 0x0 zigbee_eth_bridge-<version>-full.bin
```

To build it yourself:

```powershell
cd web; npm ci; npm run build   # writes main/web/index.html
```

Then, with ESP-IDF 5.5 set up (`export.ps1` / `export.sh` from your ESP-IDF install):

```
idf.py build
idf.py -p COM3 flash   # your board's port; first time over the Type-C, after that a release (below) or upload the .bin on System
```

UI development against a real bridge: `npm run dev` in `web/` (proxies `/api` to `BRIDGE`, default
192.168.1.50). Without a board: run `tools/mock_bridge.py 18300` (needs aiohttp and Zigbee Studio's
`dev/adapter_sim.py`: the zigbee-studio repo next to this one, or its folder as the second argument) and
`BRIDGE=http://127.0.0.1:18300 npm run dev`. `npm install` trips over a peer dependency; use `npm ci`.
Bump `version.txt` per release.

## Updates from git

The bridge reads a release manifest (Settings → Updates; default: this repo's latest GitHub release,
`https://github.com/home-controls/zigbee-bridge/releases/latest/download/manifest.json`) 15 s after boot
and every 6 hours while automatic checks are on. A newer version shows an **Update** pill in the header and
an **Install** button on System. Installing streams the image over HTTPS (CA bundle) into the spare OTA
slot, hashing as it goes, and only marks it bootable when the image header is ours (chip, project, the
version the manifest names) and the size, **MD5** and **SHA-256** all match the manifest, the image passes its
own built-in hash, and an MD5 of what's actually in flash (read back) matches again. A server that drips or
stalls is dropped (15 s per read, 5 minutes per download); redirects are followed (3 at most). Uploads and
git installs share one lock, so two can't write the spare slot at once.

**Trial boot** (upload or git): the new firmware is kept once it has run 30 s with the network up and the
dashboard serving. A crash or watchdog before that, or no network/dashboard within 2 minutes, sends the board
back to the previous firmware by itself. The old firmware then says so (event log, System page) and warns
before the same version is installed again.

Release (bump `version.txt`, `npm run build` if the UI changed, `idf.py build`):

```
python tools/publish_release.py --notes "What changed" --github
```

It refuses test builds and an image whose version doesn't match `version.txt`, writes
`zigbee_eth_bridge-<v>.bin`, its `.bin.md5` and `manifest.json` (in `release/<v>/`), plus the first-flash
`zigbee_eth_bridge-<v>-full.bin` (esptool merge_bin of `build/flash_args`, so run it in ESP-IDF's environment)
and `dongles.json` (`[]` unless that folder already has one), and makes them a GitHub release `v<v>` with
the gh CLI. To serve updates from your own git server instead, give
`--repo <checkout>` (files go to its `eth-bridge/`; `--commit --push`) and point Settings → Updates at that
manifest. Rollback: an older image has to be uploaded on System (the bridge only offers versions newer
than its own).

## Sticks the bridge knows (1.0.19)

Any USB serial adapter on J3 is driven (`usbser.c`): **CP210x**, **CH340/CH341** and **FTDI** with the same vendor requests Linux sends
(FTDI's 2 status bytes per packet stripped, latency timer 1 ms), and anything **CDC-ACM class** (CH9102F, ESP32-S3 bridges such as
Connect ZBT-2, ConBee II, nRF52840) with the standard class requests. `dongles.c` recognises ~30 models by VID:PID plus their USB
strings (half of them share 10c4:ea60), with radio, protocol, default baud, whether RTS/CTS is wired and how to reach the bootloader;
unknown adapters get a generic entry. `protodetect.c` works out the protocol from the traffic (ASH, ZNP, deCONZ, ZiGate, Spinel,
ZBOSS, each with its checksum) and the EZSP statistics only run for EmberZNet. Silicon Labs dongles can be flashed (`.gbl`), entering
the bootloader the board's way: EZSP first, then DTR/RTS, CP2102N GPIO4/5 (SLZB-07) or the ESP32-S3 baud knock (ZBT-2). TI, ConBee
and Nordic dongles run as bridges only for now. The CDC driver registers before the USB library task runs (no missed enumeration at
boot); a scan of attached devices backs that up. A new bridge firmware on trial is only kept if a dongle it identified also opens.

**Firmware check** (`fwcat.c`): reads the newest releases of a GitHub repo (default Nerivec/silabs-firmware-builder, pre-releases
included; Settings → Silicon Labs firmware source) through the API as a stream, keeps the `zigbee_ncp` builds for the dongle's board,
ranks them (bridge baud, flow control the board has; `hw_flow` refused without RTS/CTS) and downloads the chosen one into the image
area with GitHub's SHA-256 checked. Flashing a build at another baud or flow switches the bridge to it once the new firmware answers
(saved, no reboot).

**1.0.20: every family flashes, Thread, downloadable list.**
- **TI CC26x2** (`tiflash_core.c`, ROM bootloader like cc2538-bsl): Koenkk's `.zip` (unpacked with the ROM's tinfl,
  `unzip_core.c` + `tinfl_ring.h`) or a `.hex` (`ihex_core.c`) becomes a flat image in PSRAM; refused before any erase when
  its CCFG would lock the ROM bootloader out, the size or backdoor pin don't match the chip, or sectors are protected; CCFG is
  written last and the chip's CRC32 must match. Entry: ZBDongle-P gated DTR/RTS, cc2538-bsl DTR/RTS, or the BOOT button.
  Firmware check against Koenkk/Z-Stack-firmware; the running build date is read from Z-Stack's SYS_VERSION answer.
- **ConBee II/III** (`gcfflash_core.c`, gcfflasher's protocol): `.GCF` checked for the right stick (CRC-8/CRC-32 where the
  file allows), deCONZ watchdog reset into the V1/V3 bootloader, rides the ConBee II's USB re-enumeration (`dongle_wait_reopen`).
- **nRF52840 dongle** (`nrfdfu_core.c`, open USB DFU bootloader): a `.hex` becomes the `.bin` nrfutil would make and the
  bridge builds the unsigned init packet itself; enter the bootloader with the dongle's RESET button (ZBOSS builds have no
  software trigger).
- **Thread role** (Settings → Radios, radio 1 role): `openthread_rcp` builds offered/accepted, verified over Spinel after flashing
  (`nf_spinel_probe`), a running RCP asked into its bootloader with Spinel CMD_RESET; the client snippet becomes the OTBR
  add-on's `network_device: <bridge>:6638`.
- **Downloadable dongle list**: `dongles.json` next to the release manifest (fetched 30 s after boot, then daily) is checked
  before the built-in table. An array of objects: `id`, `name`, `vid`, `pid` (0 = any; number or "0x10C4"), optional
  `manufacturer`/`product` globs (product is matched against "<manufacturer> <product>"), `radio`, `family`
  (silabs|ti|deconz|nordic|esp|nxp|unknown), `protocol` (ezsp|znp|deconz|zigate|zboss|spinel|unknown), `baud`, `rtscts`,
  `reset` (none|rts_dtr|slzb07_gpio|baud_knock|ti_sonoff|ti_cc2538|deconz|nrf_dfu), `fw_board`, `note`. Bad entries are skipped.
- Images are stored as uploaded (`.gbl`, `.zip`/`.hex` TI, `.GCF`, `.hex` nRF; header magic per kind) and fully checked on
  upload; flashing goes to `xflash.c` for the non-Silicon Labs kinds and switches the bridge to the firmware's baud.

**PSRAM** (1.0.19): the P4NRW32X's 32 MB PSRAM is on (hex, 200 MHz). Allocations over 4 KB and TLS go there; `heap_*` in the API
and the watchdog's low-memory check are internal RAM, `psram_*` is reported separately.

## Radios: several sticks behind a hub (2.0)

Up to four radios, each with its own USB stick, TCP port, allowed host, role and baud, so Zigbee2MQTT, ZHA, an OpenThread
Border Router and Z-Wave JS can all run through one bridge. Radio 1 is the coordinator the bridge always had (TCP 6638,
the EZSP decoder, the device table and the deep charts); the others pass bytes straight through, with protocol
detection (Z-Wave Serial API included) and firmware version sniffing (Spinel, EZSP, Z-Stack). One client per radio,
refused/replaced the same way as radio 1's.

- **Radios set themselves up** (`radio_core.c`, `dongle.c`). A stick plugged in takes the lowest free radio and its port
  opens at once: role guessed from what the stick list says it is (Spinel: Thread; Z-Wave: Z-Wave; Zigbee: ZHA on radios
  2-4, Zigbee2MQTT on radio 1), the stick's own baud (Thread RCPs 460800), radio 1's allowed host (the same Home
  Assistant, not "anyone") and `6638 + i`, or the lowest free port. There is no on/off switch.
- **Set up, missing, freed.** A radio is *set up* once a client has connected to it (NVS `r%d_su`). Unplugging the stick of
  a radio that isn't set up frees the radio (binding gone; it disappears, nothing harmed). A set-up radio keeps its stick
  and shows "Stick missing" (state `missing`, health -15) until the stick is back or it is forgotten. A binding that is
  neither set up nor present 20 s after boot is freed too (no unplug ever comes for it). A stick being flashed may leave
  the bus (bootloader) without freeing anything.
- **Live settings.** Port, baud, allowed host, role and name apply at once: a radio's receive task rebinds its listener
  when its port changes (or closes it when the radio goes); a new baud switches the open stick (`dx_set_link_baud`) and
  the client reconnects; the mDNS `ports` TXT follows (`net_set_ports`). Only the network and `usb_fs_only` need a reboot.
  Each radio carries a `gen`, bumped when a stick takes or leaves it: a dashboard edit made before that is refused (409).
- **Hub speed.** The P4's high-speed USB port has no transaction translator, so ESP-IDF refuses full-speed devices behind a
  hub that runs at high speed (`hub.c`), and every radio stick is full speed. With `usb_fs_only` (default on) the bridge
  sets the controller's HCFG.FSLSSupp bit before the port gets power (`root_port_unpowered`, then
  `usb_host_lib_set_root_port_power`): the port never answers a hub's high-speed chirp, so a USB 2.0/3.0 hub comes up at
  full speed (12 Mbit/s shared, far above what the sticks need). A core reset after an unplug may clear the bit: a hub
  seen at high speed while it should be full speed makes the bridge set it again and power-cycle the port, at most once
  a minute (`radio_hub_reforce`). Tried on the board 2026-11-26: a USB 3.0 hub (its USB 2.0 part, 05e3:0610) came up at
  full speed with an MG24 and a ZBDongle-P behind it.
- **ZHA role.** Radios 2-4 can be a Zigbee coordinator for Home Assistant's ZHA (`zha`): flashed and detected like
  `zigbee`; the dashboard's snippet gives ZHA's radio type from the detected protocol (EZSP, ZNP, deCONZ, ZiGate; none
  for ZBOSS) and `socket://<bridge>:<port>`. Radio 1 refuses it: its dashboard view is Zigbee2MQTT's.
- **Which radio a stick belongs to.** Straight on J3 it is radio 1's, whatever it is. Behind a hub a radio keeps the stick
  it has, by VID:PID + serial, or by hub port when the serial is empty or shared; a new stick takes the lowest free radio.
  An unknown stick never takes a radio bound to another one (at boot the Thread stick must not grab Zigbee's radio
  because the Zigbee stick enumerated later). Sticks are opened by USB address, since identical sticks share a VID:PID.
- **Per radio on the dashboard.** `/api/status` lists the shown radios with `state` (`no_stick`, `missing`, `opening`,
  `ready`, `flashing`), `set_up`, `error`, `gen`; `/api/history` has `radios[].in/out`; events carry `radio` (-1 =
  the bridge). The device table only comes from radio 1's EZSP traffic.
- **Tasks and memory (2.0.1).** Every radio has its receive and send task (6 KB stacks in PSRAM, `task_start(...,
  psram)` in `tasks.c`) and a 16 KB stream in PSRAM from boot; an idle radio's tasks sleep. A task whose stack is in
  PSRAM must never write flash (the cache goes off), so the one NVS write a radio task needed (set up on the first
  client) is the dongle task's (`dx_request_set_up`). lwIP allocates its buffers in PSRAM first
  (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`; the EMAC copies into its own DMA buffers). Internal stacks are sized from
  what `GET /api/tasks` measured on the board (each task's stack, high-water mark and where it lives, plus the
  internal / DMA / PSRAM heaps; System, Memory and tasks). Radios 2-4 never hold writes on XOFF (0x13 is SendData in
  Z-Wave).
- **Limits.** 16 host channels: the hub takes 2, a CP210x/CH34x/FTDI stick 3, a CDC-ACM stick 4, so four CDC-ACM sticks
  don't fit (the radio then shows "can't open the stick ... out of channels"). One hub level
  (`CONFIG_USB_HOST_HUB_MULTI_LEVEL=n`). ESP-IDF 5.5's hub driver aborts (reboot) on some hub port errors, only logs
  hub status changes and has no port debounce: soak a hub before putting the Zigbee coordinator behind it.
- **NVS.** `r%d_*` (name, port, allow, baud, role, su), `r0_name`, `usb_fs`, saved sticks `rb0..rb3`, `rsv=2` once the
  1.x settings were moved (radios with a saved stick became set up; `r%d_on` erased; clashing ports moved).
- **Flashing and firmware checks** pick their radio (`?radio=n`); each radio keeps its own check result, for the stick it
  had then (an identical check for another radio under 10 min old is reused); one image store and one flash at a time.
  Z-Wave radios are never flashed here.

## Flashing a stick

System → **Dongle firmware**: upload a Zigbee coordinator (NCP) `.gbl` for the dongle, then **Flash the dongle**.

1. **Upload**: stored in its own 2 MB flash area (`dongle_fw` at 0x820000, after `ota_1`; boards first flashed
   with the older table register the same area at boot). The old image is dropped first and the new header is
   written last, so an interrupted upload leaves no image, never half of one. Checked before it's kept: GBL v3
   structure, every tag known, the end tag's CRC-32, firmware inside, an MD5 read back from flash. Refused: GBL
   v4, bootloader or secure-element upgrades, Thread/Bluetooth app types. Flagged (Flash stays off): metadata
   saying it's not `zigbee_ncp` (router, RCP) or a baud other than the bridge's.
2. **Flash** (`md5` must match the stored image, so it's the one on screen): the image is checked again from
   flash (header, MD5, SHA-256, structure, CRC); Zigbee2MQTT is dropped and refused until the end; the bridge
   asks EmberZNet to start its bootloader over ASH/EZSP (`launchStandaloneBootloader`), falls back to an
   already-running bootloader, then to the RTS/DTR reset pattern; sends the image over XMODEM-CRC at 115200 (bad
   blocks resent, a failed upload cancelled and retried once, not when the bootloader rejected the image);
   runs it; and only calls it done when the new firmware answers an EZSP version exchange. Baud, DTR and RTS go
   back to how Zigbee2MQTT expects them, and Zigbee2MQTT reconnects by itself.
3. **If it fails** the page says where the dongle was left: still on its old firmware (the bootloader refused
   the image before writing), or in its bootloader with no firmware. Flash again; the image stays on the bridge.
   No reboots or bridge firmware updates are accepted while it runs.

The protocol code (`main/ncpflash_core.c`) has no ESP-IDF in it: the host test drives the same file against a
simulated dongle. Same approach as universal-silabs-flasher (reset patterns, menu, XMODEM).

## Stored log

Every ESP_LOG line is stored with its UTC time in front: `2026-11-24T04:25:36.123Z I (991050) web: ...`.
Lines from before the clock is set get `boot+12.345s` (the writer holds the first lines up to 2 minutes for
the clock, so normally only a boot without network has them). The bridge adds notes of its own, marked `#`:
each boot (number, firmware, reset reason), "Clock set", lines lost if the writer ever fell behind.

- **Live log** (`logbuf.c`): the last 32 KB in memory that survives a software restart, a crash or a watchdog
  reset. The writer saves it every 2 s (sooner when 8 KB pile up); what it hadn't saved when the board went
  down is saved at the next boot under a note "its last lines, kept in memory through the reset". A power cut
  loses at most those 2 s.
- **Flash** (`logstore.c`, format in `logstore_core.c`): a ring of 4 KB sectors, each with a numbered header;
  the oldest sector is erased only when the next one is needed, so a power cut costs at most the bytes being
  written. At about 6 KB an hour a sector is erased every ~40 minutes; with 16 MB each sector is reused about
  once a year. **Where:** the firmware is built for 16 MB, and ESP-IDF limits itself to that. At boot the bridge
  reads the chip's real size; if it's bigger and the chip driver can address it (4-byte commands, Winbond/GD
  `SPI_FLASH_CHIP_CAP_32MB_SUPPORT`), it raises the limit and writes a random pattern to the last sector,
  checking it reads back and that the same spot 16 MB lower (0xFFF000, unused) didn't change, so a chip that
  ignored the top address bit could never overwrite the firmware. Passing that, the log gets the upper part
  (16 MB minus the probe sector); otherwise the 2 MB `zblog` area at 0xA20000 (in `partitions.csv`; boards
  first flashed with an older table register it at boot). **Clear** raises a floor kept in NVS instead of
  erasing 16 MB.
- **microSD** (`sdlog.c`): looked for at boot and every 30 s (the driver's own messages are muted while no
  card is in). FAT32 only (ESP-IDF has no exFAT); never formatted by the bridge. One file per UTC day,
  `zblog/2026-11-24.log`, appended and synced on every save; a write or status error (card pulled) unmounts it
  and power-cycles the slot before the next look. Under 5% (or 64 MB) free, the oldest day files are deleted.
- **Dashboard**: System → Log storage (where, how far back, how long the space lasts at the current rate,
  downloads, the card's day files) and the Log card's **Stored** tab (the newest 256 KB, previous boots included,
  with search and Warnings / Errors / Boots filters).

## Watchdogs

- **Tasks**: the two bridge tasks and the dongle task check in with the task watchdog; one that stops for
  10 s reboots the board (`CONFIG_ESP_TASK_WDT_PANIC`).
- **Memory**: free heap under 24 KB (or no 8 KB block) for 30 s restarts cleanly.
- **Network**: no Zigbee2MQTT connected, link up, and neither the Z2M host (when it's a single address) nor
  the gateway answers a ping for 10 minutes: restart. Never while Zigbee2MQTT is connected.
- The reason survives the reboot (RTC memory): event log "Restarted by the watchdog: ...", `sys.watchdog` in
  `/api/status`, System page, and Zigbee Studio's adapter card.

## Testing

| What | How |
| --- | --- |
| Protocol decoder (host) | `python test/host/test_decoder.py`: the firmware's own `ezsp_decode.c` + `stats.c` built in WSL with ASan/UBSan, fed scripted Z2M↔coordinator conversations (an independent Python ASH/EZSP encoder) byte by byte, in random chunks and in whole reads; fuzzed; device-table overflow |
| Live API stress | `python test/live_stress.py <host> <pw-file>`: realistic and overload polling, session churn, malformed input, TCP and HTTP connection floods, interrupted/oversized/stalled uploads, no reboot, memory back |
| Soak | `python test/soak.py <host> <pw-file> [minutes]`: dashboards + Studio-style polling + port probes, memory trend |
| Watchdogs | Test build (`idf.py -B build-test -D BRIDGE_TEST=1 -D SDKCONFIG=sdkconfig.test build`, version `x.y.z+test`, adds `POST /api/debug/watchdog?kind=task|heap|net`), then `python test/watchdog_test.py <host> <pw-file>` |
| Update over the network | `python tools/ota_install.py <host> <pw-file> [image]` |
| Broken updates | `python tools/ota_test.py <images dir>`: an OTA test build (see the script) installs anything newer from a release server on the PC, which serves a different bad release each time: bad manifests (404, not JSON, missing/bad md5, 10 MB, hangs, redirect loop, 6 KB notes), MD5 / SHA-256 / size mismatch, truncated (both encodings), extra bytes, 404, other chip, other project, version mismatch, not an image, corrupt image with matching hashes, stall, 2 KB/s drip, a power cut mid-download, firmware that crashes at boot, firmware with no network, then a good release. Each must be refused (or rolled back) with the bridge still up on the right firmware |
| Stored log format (host) | `python test/host/test_logstore.py`: `logstore_core.c` built in WSL with ASan/UBSan on a simulated NOR flash (erase = 0xFF, writes only clear bits, overwrites reported): blank and garbage areas, wrapping with remounts, 400 rounds of power cuts mid-write and mid-erase, clear + floor, a reader lapped by the writer; stamps against Python's UTC dates |
| USB serial, dongle list, protocol detection (host) | `python test/host/test_usbser.py`: chip by VID:PID, CH340/FTDI baud divisors against Linux values, FTDI status stripping, catalogue matches for real USB strings, detection of ASH/ZNP/deCONZ/ZiGate/Spinel/Z-Wave frames and not of noise |
| Radios (host) | `python test/host/test_radio.py`: `radio_core.c` built in WSL with ASan/UBSan: which radio a stick lands on (J3 alone, a hub added, every boot order, replug on another port, sticks without serials, duplicate serials, disabled radios, Forget), per-radio settings checks, the full-speed re-force rate limit; then a mutation check (each rule broken in a copy must fail the scenarios) |
| Radios dashboard (PC) | `MOCK_RADIOS=3 python tools/mock_bridge.py 18302` + `BRIDGE=http://127.0.0.1:18302 npm run dev` in `web/`: hub with three radios, a refused stick, Forget, per-radio settings |
| Firmware catalogue (host) | `python test/host/test_fwcat.py [releases.json ...]`: file-name parser, version compare, variant scoring, and the streaming scanner on real GitHub API answers in random-sized pieces (every asset's name, size, SHA-256 and URL vs Python's json) |
| Unzip (host) | `python test/host/test_unzip.py [release.zip]`: streaming single-file zip with zlib as the inflater on a real Koenkk release, byte-for-byte; damage/CRC/truncation/encryption refused (the device's tinfl adapter is exercised by uploading a Koenkk zip to the bridge) |
| TI flasher (host) | `python test/host/test_tiflash.py`: simulated CC26x2 ROM bootloader incl. DTR/RTS entry logic, CCFG safety refusals, retries, CRC32 verify, Intel HEX parser, real Koenkk zip flashed into the simulator |
| ConBee flasher (host) | `python test/host/test_gcfflash.py`: simulated V1/V3 bootloaders and ConBee II re-enumeration, file checks incl. the real .GCF files |
| nRF52840 DFU (host) | `python -I test/host/test_nrfdfu.py [app.hex]`: simulated open DFU bootloader written from the SDK, init packets byte-identical to nrfutil |
| USB OUT pacing + XON/XOFF (host) | `python test/host/test_txpace.py`: `txpace_core.c` built in WSL with ASan/UBSan on a virtual clock: lone frames not delayed, bursts cut at ASH flags into <= 64-byte chunks with idle line after each chunk's wire time, XOFF holds until XON / `xoff_max` / the write timeout, no bytes lost or reordered. Status: `counters.flow` (XON/XOFF seen, holds, paced chunks, queue high-water, longest write); metrics `zbbridge_ncp_xoff_total`, `zbbridge_usb_out_*` |
| Dongle flasher (host) | `python test/host/test_ncpflash.py [real.gbl ...]`: `ncpflash_core.c` built in WSL with ASan/UBSan, driven over pipes against a simulated dongle on a virtual clock (EmberZNet ASH/EZSP v13/v19, Gecko bootloader menu + XMODEM + CRC-32 check, RTS/DTR). Corrupted blocks, lost/garbled ACKs, cancels, rejected images, firmware that won't start or answer, wrong baud, USB pulled, no RTS/DTR circuit; GBL checker against damaged, dangerous and real files (Sonoff, darkxst builds) |
| Dongle image store (board) | test build (`idf.py -B build-dongle-test -D BRIDGE_TEST=1 -D SDKCONFIG=<abs>/build-dongle-test/sdkconfig -D BRIDGE_EXTRA_DEFAULTS=<abs>/test/dongle/sdkconfig.store build`), then `python tools/dongle_store_test.py <real ncp .gbl>`: bad CRC, router image, cut-off transfer, too big, not a GBL, real image, survives a reset |
| Zigbee Studio side | in the zigbee-studio repo, `zigbee_studio/`: `tests/test_adapter.py`, `dev/check_adapter_contract.py` (simulator vs real firmware JSON), `dev/adapter_e2e.py` (a running Studio against a real bridge: wrong host/token, revoked token, reboot) |

The live tests create a throwaway password if the bridge is at first-boot setup; factory-reset it afterwards.
Bugs these found (all fixed in 1.0.3–1.0.5): events lost in a burst of callbacks; the history answer as a
cJSON tree ate ~400 KB (free heap fell to 116 bytes); `tcp_port` 70000 wrapped to 4464 and a long hostname
was cut and saved; a client stalling mid-body froze the web server; Studio's "can't reach" left the cause blank.
The OTA test (all 28 pass on 1.0.13) found: the update check crashed on every run (1.0.9/1.0.10: a log line used
the manifest after freeing it); redirects read the wrong body (now a fresh connection per hop). Review found:
firmware with no network was confirmed anyway; upload and git install could write the spare slot at once; no
read-back of flash; no overall download deadline; 2 KB manifest cap.

