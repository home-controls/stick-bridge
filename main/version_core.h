#pragma once
// Firmware version order (no ESP-IDF here, so test/host/test_version.py checks it on the host).
//
//   "x.y.z"         a release
//   "x.y.z-rcN"     a release candidate of x.y.z: below x.y.z, rc2 above rc1 (any "-suffix" is a pre-release;
//                   suffixes compare by their letters, then their number, so "rc10" > "rc9")
//   "x.y.z+meta"    build metadata ("+test" builds): ignored when comparing
//
// Missing parts count as 0 ("2.1" = "2.1.0"). Bridges before 2.2.0 ignored the suffix, so to them an RC
// equals its release; that's why release candidates are only published as GitHub pre-releases, which the
// default update feed (releases/latest) never serves.

// <0 when a is older than b, 0 when the same, >0 when a is newer.
int ver_cmp(const char *a, const char *b);

// True for a pre-release ("x.y.z-anything", ignoring "+meta").
int ver_is_prerelease(const char *v);
