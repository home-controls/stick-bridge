# Contributing to Stick Bridge

Thanks for helping. Bug reports, hardware reports ("stick X works / doesn't"), ideas and pull requests
are all welcome.

- **Security problems:** report them privately, see [SECURITY.md](SECURITY.md). Never in a public issue.
- **Bugs and ideas:** open an issue with the matching template.
- **Questions about your own setup:** an issue is fine too; say what you tried.

## How changes reach `main`

`main` is protected. Nobody pushes to it directly, the maintainer included:

1. Branch from `main` (`fix-…`, `feature-…`, `docs-…`), or fork the repository.
2. Open a pull request against `main`. Fill in the template, especially **how you tested it**.
3. CI has to pass: host tests (ASan + UBSan), the dashboard build check, version/changelog/workflow
   checks and the firmware build.
4. The maintainer reviews it and merges it (squash merge, so the PR title becomes the commit message).

Pull requests from forks run CI on GitHub's own runners, and only after the maintainer approves the run.

## Building and testing

You need [ESP-IDF 5.5.6](https://docs.espressif.com/projects/esp-idf/en/v5.5.6/esp32p4/get-started/)
for the firmware, Node 24 for the dashboard, and a C compiler with zlib for the host tests.

```sh
# host tests: the ESP-free cores (decoders, flashers, log store, radios) with ASan + UBSan
python3 test/host/run_all.py

# the dashboard: type check, bundle and inline it into the firmware's page
cd web && npm ci --ignore-scripts && npm run build && cd ..

# the firmware
idf.py build
```

Details (hardware, flashing over USB, the mock bridge for UI work without a board, OTA test builds) are
in [docs/TECHNICAL.md](docs/TECHNICAL.md).

## Rules of thumb

- **Commit `main/web/index.html`** whenever you change `web/`: it's the page built into the firmware, and
  CI fails if it doesn't match a fresh build.
- **Don't edit `web/src/kit/`** by hand: it's a copy of the shared UI kit
  ([zigbee-ui](https://github.com/home-controls/zigbee-ui)). Change it there, then sync it.
- **Keep risky logic in the ESP-free `*_core.c` files** and give it a host test in `test/host/`.
- **Don't rename the identifiers listed in [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md)** (the image's
  project name, mDNS service, API id, metric names): bridges in the field and Zigbee Studio rely on them.
- **No personal data** in code, tests, screenshots or logs you attach: no real IEEE addresses, network
  keys, serial numbers, MAC addresses, public IPs or passwords. Use the documented examples
  (`192.168.1.10` Home Assistant, `192.168.1.50` the bridge, `192.168.1.20` your PC). Screenshots come
  from the mock bridge (`tools/mock_bridge.py`), never from a real network.
- **Changelog:** a change users will notice gets a line in `CHANGELOG.md` under the next version. The
  maintainer bumps `version.txt` when cutting a release ([docs/RELEASING.md](docs/RELEASING.md)). Each
  version's notes stay under 500 bytes, because the bridge shows them before installing.
- **Hardware you tested on:** say which board, sticks and firmware versions. Only the CP2102N + Silicon
  Labs path is tested on real hardware by the maintainer; reports for other sticks are especially useful.

## Licence

By contributing you agree that your contribution is licensed under the project's
[MIT licence](LICENSE). Only contribute code you wrote yourself or code whose licence allows it (say so
in the PR and add it to [NOTICE.md](NOTICE.md)). Code copied from GPL projects, including the Linux
kernel, can't be accepted.

## Code of conduct

Be kind and assume good faith. See [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).
