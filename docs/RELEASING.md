# Cutting a release

GitHub is the only home of this project: development, CI, releases and the bridges' update feed all
live here. A release is a tag on `main`; GitHub Actions builds and publishes it after the maintainer
approves it. Nothing is built or uploaded by hand.

## 1. The release pull request

On a branch from `main`:

1. Bump `version.txt` (`x.y.z`, plain numbers).
2. Add `## x.y.z` to the top of `CHANGELOG.md`. That section is what the bridge shows before installing,
   so keep it **under 500 bytes**. `python3 tools/release_info.py` checks both.
3. If `web/` changed since the last release: `npm run build` in `web/` and commit `main/web/index.html`.
4. Open the PR, titled `Release x.y.z`. CI builds the firmware and keeps the release files as the run's
   `release-files` artifact for 7 days, so they can be tried before merging.

### Trying it on a real bridge first (recommended for firmware changes)

Build a test version named after the **previous** release with a suffix, so the bridge still offers
the real release afterwards (it compares only `x.y.z` and ignores a `-suffix`). Use a separate build
directory so the test version doesn't stick in the main build:

```sh
idf.py -B build-rc -DBRIDGE_VER=2.1.2-rc1 build     # testing what becomes 2.1.3
```

then upload `build-rc/zigbee_eth_bridge.bin` on **System → Firmware**. The bridge keeps it only if it
starts properly (trial boot, see [TECHNICAL.md](TECHNICAL.md#updates-from-git)). Use a throwaway
password on test bridges, never a real one.

## 2. Merge, then tag

After the PR is approved and merged (squash):

```sh
git switch main && git pull
python3 tools/release_info.py --tag vX.Y.Z --notes /tmp/notes.md
git tag -a vX.Y.Z -F /tmp/notes.md        # an annotated tag on the merge commit
git push origin vX.Y.Z
```

Only the maintainer can create `v*` tags, and tags can't be moved or deleted (repository ruleset).

## 3. The Release workflow

Pushing the tag starts **Actions → Release**:

1. **Check the release:** the tag matches `version.txt` and the changelog, and the tagged commit is on
   `main` (merged, not just pushed somewhere).
2. **CI:** host tests, dashboard check, version check and the firmware build with the release files,
   on GitHub's clean runners.
3. **Publish** waits for approval: open the run and click **Review deployments → Approve** (the
   `release` environment). Then it signs the build provenance of the `.bin` files and `manifest.json`
   and creates the GitHub release with all files and `SHA256SUMS`.

Bridges see the update at their next check (every 6 hours, or **Check now** on System).

Running **Release** by hand (Actions → Release → Run workflow) is a dry run: it builds everything and
publishes nothing.

## If a release is bad

Tags and release files can't be replaced. Instead:

- **Stop more bridges from getting it:** edit the release and tick **Set as a pre-release**. The
  previous release becomes `latest` again, so bridges no longer see the bad one. (Bridges never offer a
  version older than their own, so ones that installed it stay on it.)
- **Fix forward:** release `x.y.z+1` with the fix. Bridges that installed the bad one get it like any update.
- A bridge whose new firmware doesn't start rolls back by itself; one that starts but misbehaves can be
  given an older image by uploading it on **System → Firmware**.

## Settings this relies on

Set by [`tools/apply_repo_settings.sh`](../tools/apply_repo_settings.sh), which records every setting
and can be run again at any time: a `main` ruleset (pull request, required checks, no force-push or deletion, no bypass), a `v*`
tag ruleset (only admins create, nobody updates or deletes), the `release` environment (required
reviewer, `v*` tags only), and fork pull requests needing approval before their workflows run.
