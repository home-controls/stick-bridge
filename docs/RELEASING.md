# Cutting a release

GitHub is the only home of this project: development, CI, releases and the bridges' update feed all
live here. A release is a tag on `main`; GitHub Actions builds and publishes it after the maintainer
approves it. Nothing is built or uploaded by hand.

One pull request per release, then tags. Every release goes out as a **release candidate** first:

```
PR "Release x.y.z" (the changes + version.txt x.y.z + changelog) → merge
tag vx.y.z-rc1 → approve → pre-release → testers try it
   (a fix? one PR with it, merge, tag vx.y.z-rc2 on the new main)
tag vx.y.z on the same commit as the last RC → approve → release → every bridge is offered it
```

`version.txt` always says the version being prepared, a plain `x.y.z`. The tag decides what's published:
an `-rcN` tag builds that commit as `x.y.z-rcN` (CI passes `-DBRIDGE_VER`), the plain tag as `x.y.z`. So
the release is the same code the testers had, and needs no pull request of its own.

A release candidate is published as a GitHub **pre-release**. The bridges' default update feed
(`releases/latest`) never serves pre-releases, so only people who install an RC on purpose get it. From
2.2.0 on, a bridge running `x.y.z-rcN` counts it as older than `x.y.z`, so testers are offered the final
release like everyone else.

## 1. The release pull request

On a branch from `main`, together with the changes themselves (one PR, not one per step):

1. Set `version.txt` to the new `x.y.z`.
2. Add `## x.y.z` to the top of `CHANGELOG.md`. That section is what the bridge shows before installing,
   so keep it **under 500 bytes**; an RC shows it prefixed with "Release candidate N of x.y.z, for
   testing." (An RC can have its own `## x.y.z-rcN` section instead, for notes only testers need.)
   `python3 tools/release_info.py` checks both files.
3. If `web/` changed since the last release: `npm run build` in `web/` and commit `main/web/index.html`.
4. Open the PR, titled `Release x.y.z`. CI builds the firmware and keeps the files as the run's
   `release-files` artifact for 7 days.

## 2. Merge, then tag

After the PR is merged (squash), tag the merge commit, first as a release candidate:

```sh
git switch main && git pull
python3 tools/release_info.py --tag vX.Y.Z-rc1 --notes /tmp/notes.md
git tag -a vX.Y.Z-rc1 -F /tmp/notes.md        # an annotated tag on the merge commit
git push origin vX.Y.Z-rc1
```

When the RC has been tested, tag **the same commit** `vX.Y.Z` the same way (`git tag -a vX.Y.Z -F ...
<commit>`). Only the maintainer can create `v*` tags, and tags can't be moved or deleted (repository
ruleset).

## 3. The Release workflow

Pushing the tag starts **Actions → Release**:

1. **Check the release:** the tag matches `version.txt` and the changelog, and the tagged commit is on
   `main` (merged, not just pushed somewhere).
2. **CI:** host tests, dashboard check, version check and the firmware build with the release files,
   on GitHub's clean runners.
3. **Publish** waits for approval: open the run, click **Review deployments**, tick `release`, **Approve
   and deploy** (never "Start all waiting jobs", which skips the approval). Then it signs the build
   provenance of the `.bin` files and `manifest.json` and creates the GitHub release with all files and
   `SHA256SUMS`: a **pre-release** for `-rcN` tags, the **latest** release otherwise.

Running **Release** by hand (Actions → Release → Run workflow) is a dry run: it builds everything and
publishes nothing.

## 4. Testing a release candidate

On a test bridge, either:

- **Upload it:** download `zigbee_eth_bridge-x.y.z-rcN.bin` from the pre-release and upload it on
  **System → Firmware**; or
- **Update to it:** set **Settings → Updates → source** to
  `https://github.com/home-controls/stick-bridge/releases/download/vx.y.z-rcN/manifest.json` and install
  from **System**. Afterwards set the source back to the default,
  `https://github.com/home-controls/stick-bridge/releases/latest/download/manifest.json`, or the bridge
  keeps checking that one RC.

The bridge keeps new firmware only if it starts properly (trial boot, see
[TECHNICAL.md](TECHNICAL.md#updates-from-git)), and rolls back by itself otherwise. Use a throwaway
password on test bridges, never a real one.

Found a problem? Fix it in one PR (version.txt stays `x.y.z`), merge, and tag the new `main` commit
`vx.y.z-rc2`. Happy with it? Tag the commit of the last RC `vx.y.z` and approve: that's the release every
bridge sees at its next check (every 6 hours, or **Check now** on System).

### Local test builds (not published)

For a quick try before an RC, build with a version that can't be confused with a published one, in a
separate build directory so it doesn't stick in the main build:

```sh
idf.py -B build-test -DSDKCONFIG=build-test/sdkconfig -DBRIDGE_VER=2.2.0-dev1 build
```

`-dev1` sorts below `-rc1` and below the release, so the bridge is offered the real versions afterwards.
`-DSDKCONFIG=...` builds from `sdkconfig.defaults` like CI does, rather than your local `sdkconfig`. In
PowerShell, quote each `-D...` option (`"-DBRIDGE_VER=2.2.0-dev1"`), or PowerShell splits it at the first
dot. Upload `build-test/zigbee_eth_bridge.bin` on **System → Firmware**.

## If a release is bad

Tags and release files can't be replaced. Instead:

- **Stop more bridges from getting it:** edit the release and tick **Set as a pre-release**. The
  previous release becomes `latest` again, so bridges no longer see the bad one. (Bridges never offer a
  version older than their own, so ones that installed it stay on it.)
- **Fix forward:** release `x.y.z+1` with the fix (through an RC if there's time). Bridges that
  installed the bad one get it like any update.
- A bridge whose new firmware doesn't start rolls back by itself; one that starts but misbehaves can be
  given an older image by uploading it on **System → Firmware**.

## Settings this relies on

Set by [`tools/apply_repo_settings.sh`](../tools/apply_repo_settings.sh), which records every setting
and can be run again at any time: a `main` ruleset (pull request, required checks, no force-push or deletion, no bypass), a `v*`
tag ruleset (only admins create, nobody updates or deletes), the `release` environment (required
reviewer, `v*` tags only), and fork pull requests needing approval before their workflows run.
