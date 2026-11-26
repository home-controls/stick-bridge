#include "dongles.h"

#include <stdio.h>
#include <string.h>

// Order matters: the first entry that matches wins, so specific strings come before generic ones on the
// same VID:PID (e.g. "*plus*mg24*" before "*sonoff*plus*"). Product globs are tried against
// "<manufacturer> <product>", the way Home Assistant's "description" matchers work.
// Sources: Home Assistant's zha / homeassistant_sky_connect / homeassistant_connect_zbt2 USB matchers,
// Zigbee2MQTT's adapter pages, universal-silabs-flasher, cc2538-bsl, Nerivec/silabs-firmware-builder
// manifests (fw_board) and Koenkk/Z-Stack-firmware (TI file prefixes). Entries marked "(unverified)"
// rest on reports, not on a device we've seen.
#define U "Strings unverified: identified by its USB name"
static const dongle_model_t MODELS[] = {
    // ---------------------------------------------------------------- SONOFF
    {"sonoff_dongle-pmg24", "SONOFF Dongle Plus MG24", 0x10C4, 0xEA60, NULL, "*plus*mg24*", "EFR32MG24", FAM_SILABS,
     PROTO_EZSP, 115200, false, RST_RTS_DTR, "sonoff_dongle-pmg24", "Software flow control only (no RTS/CTS)"},
    {"sonoff_dongle-lmg21", "SONOFF Dongle Lite MG21", 0x10C4, 0xEA60, NULL, "*lite*mg21*", "EFR32MG21", FAM_SILABS,
     PROTO_EZSP, 115200, false, RST_RTS_DTR, "sonoff_dongle-lmg21", NULL},
    {"sonoff_dongle-m", "SONOFF Dongle Max (USB)", 0x10C4, 0xEA60, NULL, "*sonoff*max*", "EFR32MG24", FAM_SILABS,
     PROTO_EZSP, 115200, false, RST_RTS_DTR, "sonoff_dongle-m", NULL},
    {"sonoff_zbdongle-p", "SONOFF ZBDongle-P", 0x10C4, 0xEA60, NULL, "*sonoff*plus*", "CC2652P", FAM_TI, PROTO_ZNP, 115200,
     false, RST_TI_SONOFF, "CC1352P2_CC2652P_launchpad", NULL},
    {"sonoff_zbdongle-e", "SONOFF ZBDongle-E", 0x1A86, 0x55D4, NULL, "*sonoff*plus*", "EFR32MG21", FAM_SILABS, PROTO_EZSP,
     115200, false, RST_RTS_DTR, "sonoff_zbdonglee", "Software flow control only (no RTS/CTS)"},
    {"sonoff_dongle-mp10", "SONOFF Dongle Max CC2674P10", 0x303A, 0x4001, NULL, "*sonoff*max*cc2674p10*", "CC2674P10",
     FAM_TI, PROTO_ZNP, 115200, false, RST_NONE, NULL, "ESP32-S3 USB bridge; flashing not supported here"},
    // ---------------------------------------------------------------- Home Assistant / Nabu Casa
    {"nabucasa_skyconnect", "Home Assistant SkyConnect", 0x10C4, 0xEA60, "*nabu casa*", "*skyconnect*", "EFR32MG21",
     FAM_SILABS, PROTO_EZSP, 115200, true, RST_NONE, "nabucasa_skyconnect", NULL},
    {"nabucasa_skyconnect", "Home Assistant Connect ZBT-1", 0x10C4, 0xEA60, "*nabu casa*", "*zbt-1*", "EFR32MG21",
     FAM_SILABS, PROTO_EZSP, 115200, true, RST_NONE, "nabucasa_skyconnect", NULL},
    {"nabucasa_zbt-2", "Home Assistant Connect ZBT-2", 0x303A, 0x4001, NULL, "*zbt-2*", "EFR32MG24", FAM_SILABS, PROTO_EZSP,
     460800, true, RST_BAUD_KNOCK, "nabucasa_zbt-2", "ESP32-S3 USB bridge"},
    {"nabucasa_zbt-2", "Home Assistant Connect ZBT-2", 0x303A, 0x831A, NULL, NULL, "EFR32MG24", FAM_SILABS, PROTO_EZSP,
     460800, true, RST_BAUD_KNOCK, "nabucasa_zbt-2", "ESP32-S3 USB bridge"},
    // ---------------------------------------------------------------- SMLIGHT (USB product strings unverified)
    {"smlight_slzb07Mg24", "SMLIGHT SLZB-07Mg24", 0x10C4, 0xEA60, NULL, "*slzb-07mg24*", "EFR32MG24", FAM_SILABS,
     PROTO_EZSP, 115200, true, RST_SLZB07_GPIO, "smlight_slzb07Mg24", U},
    {"smlight_slzb07Mg26", "SMLIGHT SLZB-07Mg26", 0x10C4, 0xEA60, NULL, "*slzb-07mg26*", "EFR32MG26", FAM_SILABS,
     PROTO_EZSP, 115200, true, RST_SLZB07_GPIO, "smlight_slzb07Mg26", U},
    {"smlight_slzb07p7", "SMLIGHT SLZB-07p7", 0x10C4, 0xEA60, NULL, "*slzb-07p7*", "CC2652P7", FAM_TI, PROTO_ZNP, 115200,
     false, RST_TI_CC2538, "CC1352P7", U},
    {"smlight_slzb07", "SMLIGHT SLZB-07", 0x10C4, 0xEA60, NULL, "*slzb-07*", "EFR32MG21", FAM_SILABS, PROTO_EZSP, 115200,
     true, RST_SLZB07_GPIO, "smlight_slzb07", NULL},
    {"smlight_slzb06Mg24", "SMLIGHT SLZB-06Mg24 (USB)", 0x10C4, 0xEA60, NULL, "*slzb-06mg24*", "EFR32MG24", FAM_SILABS,
     PROTO_EZSP, 115200, false, RST_NONE, "smlight_slzb06Mg24", U},
    {"smlight_slzb06Mg26", "SMLIGHT SLZB-06Mg26 (USB)", 0x10C4, 0xEA60, NULL, "*slzb-06mg26*", "EFR32MG26", FAM_SILABS,
     PROTO_EZSP, 115200, false, RST_NONE, "smlight_slzb06Mg26", U},
    {"smlight_slzb06m", "SMLIGHT SLZB-06M (USB)", 0x10C4, 0xEA60, NULL, "*slzb-06m*", "EFR32MG21", FAM_SILABS, PROTO_EZSP,
     115200, false, RST_NONE, "smlight_slzb06m", U},
    {"smlight_slzb06p7", "SMLIGHT SLZB-06p7 (USB)", 0x10C4, 0xEA60, NULL, "*slzb-06p7*", "CC2652P7", FAM_TI, PROTO_ZNP,
     115200, false, RST_NONE, "CC1352P7", U},
    {"smlight_slzb06", "SMLIGHT SLZB-06 (USB)", 0x10C4, 0xEA60, NULL, "*slzb-06*", "CC2652P", FAM_TI, PROTO_ZNP, 115200,
     false, RST_NONE, "CC1352P2_CC2652P_other", U},
    // ---------------------------------------------------------------- others on CP210x
    {"aeotec_zga008", "Aeotec Zi-Stick", 0x10C4, 0xEA60, "*aeotec*", NULL, "EFR32MG21", FAM_SILABS, PROTO_EZSP, 115200, true,
     RST_NONE, "aeotec_zga008", U},
    {"tubeszb_mgm24", "TubesZB MGM24 USB", 0x10C4, 0xEA60, NULL, "*tubeszb*mg*24*", "MGM240P", FAM_SILABS, PROTO_EZSP,
     115200, false, RST_NONE, "tubeszb_mgm24", U},
    {"tubeszb_cc2652", "TubesZB CC2652 USB", 0x10C4, 0xEA60, NULL, "*tubeszb*", "CC2652P", FAM_TI, PROTO_ZNP, 115200, false,
     RST_TI_CC2538, "CC1352P2_CC2652P_other", U},
    {"slaesh_cc2652rb", "slae.sh CC2652RB stick", 0x10C4, 0xEA60, NULL, "*2652*", "CC2652RB", FAM_TI, PROTO_ZNP, 115200,
     false, RST_TI_CC2538, "CC2652RB", NULL},
    // ---------------------------------------------------------------- CH340 / CH9102F
    {"zigstar_stick", "ZigStar Stick", 0x1A86, 0x7523, NULL, "*zigstar*", "CC2652P", FAM_TI, PROTO_ZNP, 115200, false,
     RST_TI_CC2538, "CC1352P2_CC2652P_other", NULL},
    {"tubeszb_cc2652", "TubesZB CC2652 USB", 0x1A86, 0x7523, NULL, "*tubeszb*", "CC2652P", FAM_TI, PROTO_ZNP, 115200, false,
     RST_TI_CC2538, "CC1352P2_CC2652P_other", NULL},
    {"easyiot_zb-gw04", "Easyiot ZB-GW04", 0x1A86, 0x7523, NULL, "*gw04*", "EFR32MG21", FAM_SILABS, PROTO_EZSP, 115200,
     false, RST_NONE, "easyiot_zb-gw04-1v1", U},
    {"easyiot_zb-gw04", "Easyiot ZB-GW04", 0x1A86, 0x55D4, NULL, "*gw04*", "EFR32MG21", FAM_SILABS, PROTO_EZSP, 115200,
     false, RST_NONE, "easyiot_zb-gw04-1v2", U},
    // ---------------------------------------------------------------- dresden elektronik / ZiGate (FTDI, native)
    {"conbee_3", "ConBee III", 0x0403, 0x6015, NULL, "*conbee*", "EFR32MG21", FAM_DECONZ, PROTO_DECONZ, 115200, false,
     RST_DECONZ, "ConBeeIII", "deCONZ firmware (.GCF); flashing not supported here yet"},
    {"zigate_v2", "ZiGate+ V2", 0x0403, 0x6015, NULL, "*zigate*", "JN5189", FAM_NXP, PROTO_ZIGATE, 115200, false, RST_NONE,
     NULL, "Unmaintained in Zigbee2MQTT"},
    {"conbee_2", "ConBee II", 0x1CF1, 0x0030, NULL, NULL, "ATSAMR21", FAM_DECONZ, PROTO_DECONZ, 115200, false, RST_DECONZ,
     "ConBeeII", "deCONZ firmware (.GCF); flashing not supported here yet"},
    // ---------------------------------------------------------------- Nordic / Espressif (native USB)
    {"nrf52840_dfu", "nRF52840 in its DFU bootloader", 0x1915, 0x521F, NULL, NULL, "nRF52840", FAM_NORDIC, PROTO_UNKNOWN,
     115200, false, RST_NRF_DFU, NULL, "In the bootloader: reset it to run its firmware"},
    {"nrf52840", "nRF52840 dongle", 0x1915, 0, NULL, NULL, "nRF52840", FAM_NORDIC, PROTO_ZBOSS, 115200, false, RST_NRF_DFU,
     NULL, "ZBOSS is experimental in Zigbee2MQTT"},
    {"esp32_zboss", "ESP32-C6/H2 (USB serial)", 0x303A, 0x1001, NULL, NULL, "ESP32-C6/H2", FAM_ESP, PROTO_ZBOSS, 115200,
     false, RST_NONE, NULL, "Assumed ZBOSS NCP: any ESP32 with USB-serial looks the same"},
    {NULL},
};

// Generic entries when nothing in the table matches: we still drive the USB chip.
static const dongle_model_t GENERIC[] = {
    {"generic_cp210x", "Unknown adapter (CP210x)", 0, 0, NULL, NULL, "", FAM_UNKNOWN, PROTO_UNKNOWN, 0, false, RST_NONE, NULL,
     "Not in the dongle list: protocol is detected from the traffic"},
    {"generic_ch34x", "Unknown adapter (CH340)", 0, 0, NULL, NULL, "", FAM_UNKNOWN, PROTO_UNKNOWN, 0, false, RST_NONE, NULL,
     "Not in the dongle list: protocol is detected from the traffic"},
    {"generic_ftdi", "Unknown adapter (FTDI)", 0, 0, NULL, NULL, "", FAM_UNKNOWN, PROTO_UNKNOWN, 0, false, RST_NONE, NULL,
     "Not in the dongle list: protocol is detected from the traffic"},
    {"generic_cdc", "Unknown adapter (USB CDC)", 0, 0, NULL, NULL, "", FAM_UNKNOWN, PROTO_UNKNOWN, 0, false, RST_NONE, NULL,
     "Not in the dongle list: protocol is detected from the traffic"},
    {"unsupported", "Unsupported USB device", 0, 0, NULL, NULL, "", FAM_UNKNOWN, PROTO_UNKNOWN, 0, false, RST_NONE, NULL,
     "Not a USB serial adapter the bridge can drive"},
};

const dongle_model_t *dongle_models(void) { return MODELS; }

static const dongle_model_t *volatile s_extra;
static volatile int s_nextra;

void dongles_set_extra(const dongle_model_t *models, int n) {
    s_nextra = 0;  // readers see either the old list with its count or an empty one, never a mix
    s_extra = models;
    s_nextra = n;
}
int dongles_extra_count(void) { return s_nextra; }
const dongle_model_t *dongles_extra(void) { return s_extra; }
bool dongle_is_extra(const dongle_model_t *m) {
    const dongle_model_t *e = s_extra;
    int n = s_nextra;
    return e && m >= e && m < e + n;
}

static int find_name(const char *const *names, int n, const char *s) {
    if (!s) return -1;
    for (int i = 0; i < n; i++)
        if (names[i] && strcmp(names[i], s) == 0) return i;
    return -1;
}
int proto_from_name(const char *s) {
    static const char *const N[] = {"unknown", "ezsp", "znp", "deconz", "zigate", "zboss", "spinel"};
    return find_name(N, 7, s);
}
int reset_from_name(const char *s) {
    static const char *const N[] = {"none", "rts_dtr", "slzb07_gpio", "baud_knock", "ti_sonoff", "ti_cc2538", "deconz", "nrf_dfu"};
    return find_name(N, 8, s);
}
int family_from_name(const char *s) {
    static const char *const N[] = {"unknown", "silabs", "ti", "deconz", "nordic", "esp", "nxp"};
    return find_name(N, 7, s);
}

const dongle_model_t *dongle_match(uint16_t vid, uint16_t pid, const char *mfr, const char *product, bool has_cdc) {
    char both[112];
    snprintf(both, sizeof(both), "%s %s", mfr ? mfr : "", product ? product : "");
    const dongle_model_t *ex = s_extra;
    int nex = s_nextra;
    for (int i = 0; ex && i < nex; i++) {
        const dongle_model_t *m = &ex[i];
        if (m->vid != vid || (m->pid && m->pid != pid)) continue;
        if (m->mfr && !us_glob(m->mfr, mfr)) continue;
        if (m->product && !us_glob(m->product, both)) continue;
        return m;
    }
    for (const dongle_model_t *m = MODELS; m->id; m++) {
        if (m->vid != vid || (m->pid && m->pid != pid)) continue;
        if (m->mfr && !us_glob(m->mfr, mfr)) continue;
        if (m->product && !us_glob(m->product, both)) continue;
        return m;
    }
    switch (us_chip_for(vid, pid, has_cdc)) {
        case US_CP210X: return &GENERIC[0];
        case US_CH34X: return &GENERIC[1];
        case US_FTDI: return &GENERIC[2];
        case US_CDC: return &GENERIC[3];
        default: return &GENERIC[4];
    }
}

const char *proto_name(proto_t p) {
    switch (p) {
        case PROTO_EZSP: return "EmberZNet (EZSP)";
        case PROTO_ZNP: return "Z-Stack (ZNP)";
        case PROTO_DECONZ: return "deCONZ";
        case PROTO_ZIGATE: return "ZiGate";
        case PROTO_ZBOSS: return "ZBOSS";
        case PROTO_SPINEL: return "OpenThread RCP (Spinel)";
        default: return "unknown";
    }
}

const char *proto_z2m_adapter(proto_t p) {
    switch (p) {
        case PROTO_EZSP: return "ember";
        case PROTO_ZNP: return "zstack";
        case PROTO_DECONZ: return "deconz";
        case PROTO_ZIGATE: return "zigate";
        case PROTO_ZBOSS: return "zboss";
        default: return "";
    }
}

const char *reset_name(reset_t r) {
    switch (r) {
        case RST_RTS_DTR: return "DTR/RTS reset";
        case RST_SLZB07_GPIO: return "CP2102N GPIO reset";
        case RST_BAUD_KNOCK: return "ESP32-S3 baud knock";
        case RST_TI_SONOFF: return "TI BSL (SONOFF gated DTR/RTS)";
        case RST_TI_CC2538: return "TI BSL (DTR/RTS)";
        case RST_DECONZ: return "deCONZ bootloader command";
        case RST_NRF_DFU: return "Nordic DFU";
        default: return "software only";
    }
}

const char *family_name(family_t f) {
    switch (f) {
        case FAM_SILABS: return "Silicon Labs";
        case FAM_TI: return "Texas Instruments";
        case FAM_DECONZ: return "dresden elektronik";
        case FAM_NORDIC: return "Nordic";
        case FAM_ESP: return "Espressif";
        case FAM_NXP: return "NXP";
        default: return "unknown";
    }
}
