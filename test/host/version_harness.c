// Checks the firmware's own version_core.c (test_version.py builds it with ASan/UBSan).
//
//   version_harness selftest

#include <stdio.h>
#include <string.h>

#include "version_core.h"

static int fails, checks;

static void expect(const char *a, const char *b, int want) {
    checks++;
    int got = ver_cmp(a, b), back = ver_cmp(b, a);
    int sgot = got < 0 ? -1 : got > 0, sback = back < 0 ? -1 : back > 0;
    if (sgot != want || sback != -want) {
        printf("FAIL ver_cmp(\"%s\", \"%s\") = %d (reverse %d), want %d\n", a, b, got, back, want);
        fails++;
    }
}

static void pre(const char *v, int want) {
    checks++;
    if (!!ver_is_prerelease(v) != want) {
        printf("FAIL ver_is_prerelease(\"%s\") != %d\n", v, want);
        fails++;
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    // releases
    expect("2.1.3", "2.1.3", 0);
    expect("2.1.4", "2.1.3", 1);
    expect("2.2.0", "2.1.9", 1);
    expect("10.0.0", "9.9.9", 1);
    expect("2.1.10", "2.1.9", 1);
    expect("2.1", "2.1.0", 0);
    expect("2", "2.0.0", 0);
    // release candidates: below their release, above the one before
    expect("2.2.0-rc1", "2.2.0", -1);
    expect("2.2.0-rc1", "2.1.3", 1);
    expect("2.2.0-rc2", "2.2.0-rc1", 1);
    expect("2.2.0-rc10", "2.2.0-rc9", 1);
    expect("2.2.0-rc1", "2.2.0-rc1", 0);
    expect("2.2.1-rc1", "2.2.0", 1);
    expect("2.2.0-beta1", "2.2.0-rc1", -1);  // letters first: "beta" < "rc"
    expect("2.2.0-rc", "2.2.0-rc1", -1);
    expect("2.2.0-rc1a", "2.2.0-rc1", 1);
    // build metadata is ignored
    expect("2.2.0+test", "2.2.0", 0);
    expect("2.2.0-rc1+test", "2.2.0-rc1", 0);
    expect("2.2.0-rc1+test", "2.2.0", -1);
    // the update decision the bridge makes: newer = cmp(latest, running) > 0
    expect("2.2.0", "2.2.0-rc3", 1);  // a tester on an RC is offered the release
    expect("2.2.0-rc2", "2.2.0", -1); // a bridge on the release isn't offered an older RC
    // odd input doesn't crash and has a stable order
    expect("", "0.0.0", 0);
    expect("garbage", "0.0.0", 0);
    expect("1.2.3.4", "1.2.3", 0);
    expect("99999999999999999999.0.0", "1000000000.0.0", 0);  // both saturate at 10^9 instead of overflowing
    expect("999999999.0.0", "1000000000.0.0", -1);
    checks++;
    if (ver_cmp(NULL, "0.0.0") != 0) { printf("FAIL NULL\n"); fails++; }
    pre("2.2.0-rc1", 1);
    pre("2.2.0", 0);
    pre("2.2.0+test", 0);
    pre("2.2.0-rc1+test", 1);
    printf("%d checks, %d failure(s)\n", checks, fails);
    return fails ? 1 : 0;
}
