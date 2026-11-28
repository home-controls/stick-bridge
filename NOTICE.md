# Notices

Stick Bridge's own code is MIT licensed (see [LICENSE](LICENSE)). The firmware image and the dashboard
page built into it also contain the third-party parts below, each under its own licence. Release
binaries include all of them.

## Not covered by the MIT licence

**The Zigbee logo** (`web/src/kit/assets/zigbee-logo.png`, also embedded in `main/web/index.html` and so
in every firmware image) is a trademark of the Connectivity Standards Alliance. The maintainer uses it
under a licence that is not passed on to you. It is **not** part of the MIT-licensed Software: if you
fork or redistribute this project, replace it with your own artwork.

Zigbee is a trademark of the Connectivity Standards Alliance, Thread of the Thread Group, Inc., and
Z-Wave of Silicon Laboratories, Inc. Product names and trademarks of stick makers (Silicon Labs, Texas
Instruments, Nordic Semiconductor, dresden elektronik, SONOFF/ITead and others) belong to their owners.
They are used here only to say which hardware works. This project is not affiliated with or endorsed by
any of them.

## Built into the firmware image

| Component | Version | Licence | Source |
| --- | --- | --- | --- |
| ESP-IDF (drivers, bootloader, esp_http_client, esp_https_ota, NVS, partition and OTA code) | 5.5.6 | Apache-2.0 | <https://github.com/espressif/esp-idf> |
| FreeRTOS kernel (ESP-IDF port) | as in ESP-IDF 5.5.6 | MIT | <https://github.com/espressif/esp-idf/tree/v5.5.6/components/freertos> |
| lwIP | as in ESP-IDF 5.5.6 | BSD-3-Clause | <https://savannah.nongnu.org/projects/lwip/> |
| Mbed TLS | as in ESP-IDF 5.5.6 | Apache-2.0 | <https://github.com/Mbed-TLS/mbedtls> |
| Mozilla CA certificate bundle (ESP-IDF's `esp_crt_bundle`) | as in ESP-IDF 5.5.6 | MPL-2.0 | <https://wiki.mozilla.org/CA> |
| cJSON (ESP-IDF's `json` component) | as in ESP-IDF 5.5.6 | MIT | <https://github.com/DaveGamble/cJSON> |
| FatFs (SD card) | as in ESP-IDF 5.5.6 | FatFs licence (BSD-style, 1-clause) | <http://elm-chan.org/fsw/ff/> |
| newlib (C library) | as in ESP-IDF 5.5.6 | BSD and similar permissive licences | <https://sourceware.org/newlib/> |
| miniz `tinfl` inflater (in the ESP32-P4 ROM, called through `main/tinfl_ring.h`) | ROM | MIT | <https://github.com/richgel999/miniz> |
| espressif/mdns | 1.15.1 | Apache-2.0 | <https://components.espressif.com/components/espressif/mdns> |
| espressif/usb_host_cdc_acm | 2.4.1 | Apache-2.0 | <https://components.espressif.com/components/espressif/usb_host_cdc_acm> |

ESP-IDF's own list of the code it contains, with every licence, is its
[COPYRIGHT.rst](https://github.com/espressif/esp-idf/blob/v5.5.6/COPYRIGHT.rst). The Apache-2.0 and
MPL-2.0 texts are at <https://www.apache.org/licenses/LICENSE-2.0> and <https://mozilla.org/MPL/2.0/>.

## Built into the dashboard page

The dashboard is bundled and minified into `main/web/index.html`, which removes the packages' own
licence comments, so they are listed here.

| Package | Version | Licence | Copyright |
| --- | --- | --- | --- |
| preact | 11.2.0 | MIT | Copyright (c) 2015-present Jason Miller |
| @preact/signals | 2.11.4 | MIT | Copyright (c) 2022-present Preact Team |
| @preact/signals-core | 1.14.5 | MIT | Copyright (c) 2022-present Preact Team |

The build tools in `web/package.json`'s devDependencies (Vite, TypeScript, the Preact preset) don't end
up in the firmware.

## Protocols and formats

The stick flashers and decoders were written for this project. Protocol constants, frame layouts and
command sequences come from these projects and documents, credited with thanks. Where a file says it
follows one of the BSD-licensed projects, that project's notice is reproduced at the end of this file.

- [gcfflasher](https://github.com/dresden-elektronik/gcfflasher) (BSD-3-Clause): the ConBee GCF bootloader protocol (`main/gcfflash_core.c`)
- [cc2538-bsl](https://github.com/JelmerT/cc2538-bsl) (BSD-3-Clause): the TI ROM bootloader protocol (`main/tiflash_core.c`)
- [zigbee-herdsman](https://github.com/Koenkk/zigbee-herdsman) (MIT): EZSP frame layouts (`main/ezsp_decode.c`)
- [Z-Stack-firmware](https://github.com/Koenkk/Z-Stack-firmware) (MIT) and [silabs-firmware-builder](https://github.com/NabuCasa/silabs-firmware-builder): the firmware catalogues the bridge reads at run time
- [universal-silabs-flasher](https://github.com/NabuCasa/universal-silabs-flasher): bootloader entry timings for some Silicon Labs sticks
- Nordic Semiconductor's nRF5 SDK documentation: the Secure DFU serial protocol (`main/nrfdfu_core.c`)
- The chip makers' documents (WCH CH340/CH341, FTDI AN232B-05, Silicon Labs AN571): USB serial adapter requests (`main/usbser.c`). The CH340/CH341 and FTDI baud-rate code (`main/serial_baud.c`) was implemented clean-room, from [docs/usb-serial-baud.md](docs/usb-serial-baud.md) alone, without access to the previous code or any other driver's source

## Notices required by BSD-3-Clause

### gcfflasher (`main/gcfflash_core.c`)

```text
Copyright (c) 2021, dresden elektronik ingenieurtechnik gmbh

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### cc2538-bsl (`main/tiflash_core.c`)

```text
BSD 3-Clause License

Copyright (c) 2014, Jelmer Tiete <jelmer@tiete.be>.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
