# Names that stay the same on purpose

The project was called **Zigbee Bridge** (`home-controls/zigbee-bridge`) until 2.1.2 and is now **Stick
Bridge** (`home-controls/stick-bridge`), because it bridges any radio stick that talks over serial. The
names users read changed. The identifiers below did **not**, because bridges in the field, Zigbee Studio,
Prometheus setups and people's Zigbee2MQTT configs depend on them. Don't rename them in a pull request;
changing one needs a planned migration (see the tracking issue linked from the README's issues).

| Identifier | Where | Who depends on it | Why it stays |
| --- | --- | --- | --- |
| Firmware project name `zigbee_eth_bridge` | `CMakeLists.txt` `project()`, image header | every running bridge | `main/fwcheck.c` refuses an update or upload whose project name differs from the running firmware's. A new name would make every bridge refuse all later releases. It could only change after a release that accepts both names has reached the bridges. |
| Release file names `zigbee_eth_bridge-<v>.bin`, `.bin.md5`, `-full.bin` | release assets | docs, people's scripts | Bridges find the image through `manifest.json`'s `file`, so these could change, but they match the project name above and people's first-flash instructions. |
| `manifest.json`, `dongles.json` | `releases/latest/download/` | every running bridge | The bridge fetches exactly these names next to its update URL. |
| Old update URL `github.com/home-controls/zigbee-bridge/releases/latest/download/manifest.json` | saved in each bridge's settings | bridges set up before the rename | A bridge saves its update URL the first time any setting is saved, so it keeps the old one. GitHub redirects it (301 to the new name, then its usual two download redirects: three in all). Bridges from 1.0.22 follow up to three redirects, from 2.1.1 up to four. **Nobody may ever create a new repository named `home-controls/zigbee-bridge`**: that would break the redirect and every old bridge's updates. |
| mDNS service `_zbbridge._tcp`, TXT `api=zbbridge/1`, `zigbee_port`, `ports` | `main/net.c` | Zigbee Studio finds bridges by it | Studio's discovery and API contract. |
| API id `"api": "zbbridge/1"` | `/api/status` (`main/web.c`) | Zigbee Studio | Same contract. |
| Metric names `zbbridge_*`, `zbbridge_radio_*` | `/metrics` | Prometheus dashboards and alerts | Renaming breaks every query. |
| Default hostname `zigbee-eth-bridge` (`zigbee-eth-bridge.local`) | `main/Kconfig.projbuild`, `sdkconfig.defaults` | Zigbee2MQTT configs using the `.local` name | Saved per bridge; a new default would only affect new or factory-reset bridges, and break copied instructions. |
| Basic auth realm and User-Agent `zigbee-eth-bridge` | `main/web.c`, `main/updater.c`, `main/fwcat.c` | nothing important | Cosmetic; left alone to keep this change small. |

What changed with the rename: the repository URL, the default update URL compiled into new firmware,
the product name in the dashboard, release titles and docs, and the names of downloaded log files.
