// Host tests for usbser_core.c, dongles.c and protodetect.c (built by test_usbser.py with ASan/UBSan).
//   usbser_harness selftest
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dongles.h"
#include "protodetect.h"
#include "usbser_core.h"

static int g_fail;
#define CHECK(c, ...)                                   \
    do {                                                \
        if (!(c)) {                                     \
            printf("FAIL %s:%d: ", __func__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
            g_fail++;                                   \
        }                                               \
    } while (0)

static double err_pct(uint32_t want, uint32_t got) { return 100.0 * ((double)got - want) / want; }

static void t_chip_for(void) {
    CHECK(us_chip_for(0x10C4, 0xEA60, false) == US_CP210X, "cp2102n");
    CHECK(us_chip_for(0x1A86, 0x7523, false) == US_CH34X, "ch340");
    CHECK(us_chip_for(0x1A86, 0x55D4, false) == US_CDC, "ch9102f is cdc");
    CHECK(us_chip_for(0x0403, 0x6015, false) == US_FTDI, "ft-x");
    CHECK(us_chip_for(0x1CF1, 0x0030, true) == US_CDC, "conbee ii cdc");
    CHECK(us_chip_for(0x1234, 0x5678, false) == US_NONE, "unknown, no cdc");
}

static void t_ch34x(void) {
    const uint32_t rates[] = {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1000000, 2000000};
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        int w = us_ch34x_divisor(rates[i], 0x30);
        CHECK(w >= 0, "%u has a divisor", rates[i]);
        CHECK(w & 0x80, "%u: bit 7 set for version 0x30", rates[i]);
        uint32_t got = us_ch34x_actual(w);
        double e = err_pct(rates[i], got);
        CHECK(e > -2.0 && e < 2.0, "%u -> %u (%.2f%%)", rates[i], got, e);
    }
    CHECK((us_ch34x_divisor(115200, 0x27) & 0x80) == 0, "no bit 7 for old chips");
    // Linux gives 0xcc03 for 115200 (div 0x100-0xcc=52 at ps 3 fact 0: 48e6/(8*52)=115384)
    CHECK((us_ch34x_divisor(115200, 0) & 0xFF7F) == 0xCC03, "115200 word %04x", us_ch34x_divisor(115200, 0));
}

static void t_ftdi(void) {
    CHECK(us_ftdi_divisor(115200) == 0x001A, "115200 -> %x", us_ftdi_divisor(115200));
    CHECK(us_ftdi_divisor(460800) == 0x4006, "460800 -> %x", us_ftdi_divisor(460800));
    CHECK(us_ftdi_divisor(921600) == 0x8003, "921600 -> %x", us_ftdi_divisor(921600));
    CHECK(us_ftdi_divisor(3000000) == 0, "3M");
    CHECK(us_ftdi_divisor(2000000) == 1, "2M");
    CHECK(us_ftdi_divisor(9600) == 0x4138, "9600 -> %x", us_ftdi_divisor(9600));
    const uint32_t rates[] = {9600, 57600, 115200, 230400, 460800, 921600, 1000000};
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        double e = err_pct(rates[i], us_ftdi_actual(us_ftdi_divisor(rates[i])));
        CHECK(e > -3.5 && e < 3.5, "%u -> %u (%.2f%%)", rates[i], us_ftdi_actual(us_ftdi_divisor(rates[i])), e);
    }
    // two full packets and a short one: 64 + 64 + 5 bytes -> 62 + 62 + 3 payload
    uint8_t buf[133];
    for (int i = 0; i < 133; i++) buf[i] = (uint8_t)i;
    buf[0] = 0x01; buf[1] = 0x60;
    buf[64] = 0x11; buf[65] = 0x62;  // CTS + overrun
    buf[128] = 0x01; buf[129] = 0x60;
    uint8_t modem = 0, errs = 0;
    size_t n = us_ftdi_strip(buf, sizeof(buf), 64, &modem, &errs);
    CHECK(n == 127, "payload %zu", n);
    CHECK(buf[0] == 2 && buf[61] == 63 && buf[62] == 66 && buf[124] == 130 && buf[126] == 132, "bytes in order");
    CHECK(modem == 0x01 && (errs & US_FTDI_LINE_OVERRUN), "status kept (modem %02x errs %02x)", modem, errs);
    uint8_t empty[2] = {0x01, 0x60};
    CHECK(us_ftdi_strip(empty, 2, 64, NULL, NULL) == 0, "status-only packet");
}

static void t_glob(void) {
    CHECK(us_glob("*sonoff*plus*", "ITead Sonoff Zigbee 3.0 USB Dongle Plus"), "ha style");
    CHECK(us_glob("*PLUS*MG24*", "sonoff dongle plus mg24"), "case");
    CHECK(!us_glob("*slzb-07mg24*", "SMLIGHT SLZB-07"), "no");
    CHECK(us_glob("a?c", "abc") && !us_glob("a?c", "ac"), "?");
    CHECK(us_glob(NULL, "x") && us_glob("*", ""), "null/empty");
}

static void t_match(void) {
    struct { uint16_t v, p; const char *m, *pr; bool cdc; const char *want; } cases[] = {
        {0x10C4, 0xEA60, "SONOFF", "SONOFF Dongle Plus MG24", false, "sonoff_dongle-pmg24"},
        {0x10C4, 0xEA60, "ITead", "Sonoff Zigbee 3.0 USB Dongle Plus", false, "sonoff_zbdongle-p"},
        {0x1A86, 0x55D4, "", "SONOFF Zigbee 3.0 USB Dongle Plus V2", true, "sonoff_zbdongle-e"},
        {0x10C4, 0xEA60, "SONOFF", "SONOFF Dongle Lite MG21", false, "sonoff_dongle-lmg21"},
        {0x10C4, 0xEA60, "Nabu Casa", "SkyConnect v1.0", false, "nabucasa_skyconnect"},
        {0x10C4, 0xEA60, "Nabu Casa", "Home Assistant Connect ZBT-1", false, "nabucasa_skyconnect"},
        {0x303A, 0x4001, "Nabu Casa", "ZBT-2", true, "nabucasa_zbt-2"},
        {0x303A, 0x4001, "SONOFF", "SONOFF Dongle Max CC2674P10", true, "sonoff_dongle-mp10"},
        {0x10C4, 0xEA60, "SMLIGHT", "SLZB-07Mg24", false, "smlight_slzb07Mg24"},
        {0x10C4, 0xEA60, "SMLIGHT", "SLZB-07", false, "smlight_slzb07"},
        {0x0403, 0x6015, "dresden elektronik", "ConBee III", false, "conbee_3"},
        {0x1CF1, 0x0030, "dresden elektronik ingenieurtechnik GmbH", "ConBee II", true, "conbee_2"},
        {0x1A86, 0x7523, "", "USB Serial", false, "generic_ch34x"},
        {0x10C4, 0xEA60, "Silicon Labs", "CP2102N USB to UART Bridge Controller", false, "generic_cp210x"},
        {0x0403, 0x6015, "FTDI", "FT230X Basic UART", false, "generic_ftdi"},
        {0x1234, 0x5678, "x", "y", true, "generic_cdc"},
        {0x1234, 0x5678, "x", "y", false, "unsupported"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const dongle_model_t *m = dongle_match(cases[i].v, cases[i].p, cases[i].m, cases[i].pr, cases[i].cdc);
        CHECK(m && strcmp(m->id, cases[i].want) == 0, "'%s %s' -> %s (want %s)", cases[i].m, cases[i].pr, m ? m->id : "NULL",
              cases[i].want);
    }
    // Every Silicon Labs entry names its firmware board; no TI entry claims a Silabs board.
    for (const dongle_model_t *m = dongle_models(); m->id; m++) {
        if (m->family == FAM_SILABS) CHECK(m->fw_board && m->proto == PROTO_EZSP, "%s: silabs needs fw_board + EZSP", m->id);
        CHECK(m->name && m->radio, "%s: name/radio", m->id);
    }
}

static proto_t feed_all(pd_state_t *s, int dir, const uint8_t *d, size_t n) { return pd_feed(s, dir, d, n); }

static void t_detect(void) {
    pd_state_t s;
    // ASH: host RST, NCP RSTACK (canonical frames)
    const uint8_t rst[] = {0x1A, 0xC0, 0x38, 0xBC, 0x7E};
    const uint8_t rstack[] = {0xC1, 0x02, 0x02, 0x9B, 0x7B, 0x7E};
    pd_reset(&s);
    CHECK(feed_all(&s, 0, rst, sizeof(rst)) == PROTO_UNKNOWN, "one frame isn't enough");
    CHECK(feed_all(&s, 1, rstack, sizeof(rstack)) == PROTO_EZSP, "ash");
    // XON/XOFF between ASH bytes don't break framing
    const uint8_t rst_x[] = {0xC0, 0x11, 0x38, 0xBC, 0x13, 0x7E, 0xC0, 0x38, 0xBC, 0x7E};
    pd_reset(&s);
    CHECK(feed_all(&s, 0, rst_x, sizeof(rst_x)) == PROTO_EZSP, "ash with xon/xoff");
    // ZNP: SYS_PING request + SRSP (FE 02 61 01 59 06 3D)
    const uint8_t ping[] = {0xFE, 0x00, 0x21, 0x01, 0x20};
    const uint8_t pong[] = {0xFE, 0x02, 0x61, 0x01, 0x59, 0x06, 0x3D};
    pd_reset(&s);
    feed_all(&s, 0, ping, sizeof(ping));
    CHECK(feed_all(&s, 1, pong, sizeof(pong)) == PROTO_ZNP, "znp");
    // deCONZ: VERSION request 0D seq 00 0900 00000000 + crc (two's complement sum)
    uint8_t dz[12] = {0xC0, 0x0D, 0x01, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0, 0};
    uint16_t sum = 0;
    for (int i = 1; i < 10; i++) sum += dz[i];
    uint16_t crc = (uint16_t)(~sum + 1);
    dz[10] = (uint8_t)crc;
    dz[11] = (uint8_t)(crc >> 8);
    uint8_t dz2[26];
    memcpy(dz2, dz, 12);
    memcpy(dz2 + 12, dz, 12);
    dz2[24] = 0xC0;
    pd_reset(&s);
    CHECK(feed_all(&s, 0, dz2, 25) == PROTO_DECONZ, "deconz");
    // ZiGate: get version 0x0010, len 0, chk 0x10 -> 01 02 10 10 02 10 02 10 10 03 (escaped)
    const uint8_t zg[] = {0x01, 0x02, 0x10, 0x10, 0x02, 0x10, 0x02, 0x10, 0x10, 0x03,
                          0x01, 0x02, 0x10, 0x10, 0x02, 0x10, 0x02, 0x10, 0x10, 0x03};
    pd_reset(&s);
    CHECK(feed_all(&s, 0, zg, sizeof(zg)) == PROTO_ZIGATE, "zigate");
    // Spinel: frame "81 02 02" + X.25 CRC (computed here bitwise, independently of protodetect.c)
    uint8_t sp[] = {0x7E, 0x81, 0x02, 0x02, 0, 0, 0x7E};
    uint16_t c = 0xFFFF;
    for (int i = 1; i <= 3; i++) {
        c ^= sp[i];
        for (int b = 0; b < 8; b++) c = (c & 1) ? (c >> 1) ^ 0x8408 : c >> 1;
    }
    c = ~c;
    sp[4] = (uint8_t)c;
    sp[5] = (uint8_t)(c >> 8);
    pd_reset(&s);
    feed_all(&s, 1, sp, sizeof(sp));
    CHECK(feed_all(&s, 1, sp + 1, sizeof(sp) - 1) == PROTO_SPINEL, "spinel");
    // Noise must not decide anything quickly
    uint8_t noise[4096];
    uint32_t r = 12345;
    for (size_t i = 0; i < sizeof(noise); i++) {
        r = r * 1103515245u + 12345u;
        noise[i] = (uint8_t)(r >> 16);
    }
    pd_reset(&s);
    proto_t got = feed_all(&s, 0, noise, 512);
    CHECK(got == PROTO_UNKNOWN, "512 random bytes decided %d", got);
    // Once decided it sticks
    pd_reset(&s);
    feed_all(&s, 0, rst, sizeof(rst));
    feed_all(&s, 1, rstack, sizeof(rstack));
    CHECK(feed_all(&s, 0, ping, sizeof(ping)) == PROTO_EZSP, "sticky");
}

static void t_extra(void) {
    static const dongle_model_t ex[] = {
        {"my_stick", "My custom MG24 stick", 0x10C4, 0xEA60, NULL, "*plus*mg24*", "EFR32MG24", FAM_SILABS, PROTO_EZSP, 460800,
         true, RST_NONE, "my_board", NULL},
    };
    dongles_set_extra(ex, 1);
    const dongle_model_t *m = dongle_match(0x10C4, 0xEA60, "SONOFF", "SONOFF Dongle Plus MG24", false);
    CHECK(m && strcmp(m->id, "my_stick") == 0 && dongle_is_extra(m), "downloaded entry wins (%s)", m ? m->id : "NULL");
    m = dongle_match(0x10C4, 0xEA60, "Nabu Casa", "SkyConnect v1.0", false);
    CHECK(m && strcmp(m->id, "nabucasa_skyconnect") == 0 && !dongle_is_extra(m), "others still built-in");
    dongles_set_extra(NULL, 0);
    m = dongle_match(0x10C4, 0xEA60, "SONOFF", "SONOFF Dongle Plus MG24", false);
    CHECK(m && strcmp(m->id, "sonoff_dongle-pmg24") == 0, "cleared -> built-in again");
    CHECK(proto_from_name("spinel") == PROTO_SPINEL && reset_from_name("baud_knock") == RST_BAUD_KNOCK &&
          family_from_name("ti") == FAM_TI && proto_from_name("bogus") == -1, "name lookups");
}

static void t_versions(void) {
    pd_ver_t v;
    pd_ver_reset(&v);
    // Spinel VALUE_IS NCP_VERSION "SL-OPENTHREAD/2.6.1" (FCS computed independently, bitwise X.25)
    const char *ver = "SL-OPENTHREAD/2.6.1";
    uint8_t raw[64], fr[80];
    size_t n = 0, k = 0;
    raw[n++] = 0x81; raw[n++] = 0x06; raw[n++] = 0x02;
    memcpy(raw + n, ver, strlen(ver) + 1);
    n += strlen(ver) + 1;
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= raw[i];
        for (int b = 0; b < 8; b++) c = (c & 1) ? (c >> 1) ^ 0x8408 : c >> 1;
    }
    c = ~c;
    raw[n++] = (uint8_t)c;
    raw[n++] = (uint8_t)(c >> 8);
    fr[k++] = 0x7E;
    for (size_t i = 0; i < n; i++) {
        if (raw[i] == 0x7E || raw[i] == 0x7D || raw[i] == 0x11 || raw[i] == 0x13 || raw[i] == 0xF8) { fr[k++] = 0x7D; fr[k++] = raw[i] ^ 0x20; }
        else fr[k++] = raw[i];
    }
    fr[k++] = 0x7E;
    // feed byte by byte to cross every boundary
    bool learnt = false;
    for (size_t i = 0; i < k; i++) learnt |= pd_ver_feed(&v, fr + i, 1);
    CHECK(learnt && strcmp(v.spinel_version, ver) == 0, "spinel version '%s'", v.spinel_version);
    CHECK(!pd_ver_feed(&v, fr, k), "same version again: nothing new");
    // ZNP SYS_VERSION SRSP: transport 2, product 2, 2.7.1, revision 20250321 (0x0134FED1, little-endian)
    uint8_t z[] = {0xFE, 0x09, 0x61, 0x02, 0x02, 0x02, 0x02, 0x07, 0x01, 0xD1, 0xFE, 0x34, 0x01, 0x00};
    uint8_t x = 0;
    for (size_t i = 1; i < sizeof(z) - 1; i++) x ^= z[i];
    z[sizeof(z) - 1] = x;
    CHECK(pd_ver_feed(&v, z, sizeof(z)) && v.znp_seen && v.znp_revision == 20250321 && v.znp_major == 2 && v.znp_minor == 7,
          "znp revision %u %u.%u", v.znp_revision, v.znp_major, v.znp_minor);
    z[sizeof(z) - 1] ^= 1;  // bad FCS ignored
    pd_ver_t w;
    pd_ver_reset(&w);
    CHECK(!pd_ver_feed(&w, z, sizeof(z)) && !w.znp_seen, "bad fcs ignored");
}

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "selftest") != 0) return 2;
    t_chip_for();
    t_ch34x();
    t_ftdi();
    t_glob();
    t_match();
    t_detect();
    t_versions();
    t_extra();
    printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "all passed", g_fail);
    return g_fail ? 1 : 0;
}
