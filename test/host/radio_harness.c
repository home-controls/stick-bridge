// Runs the firmware's own radio_core.c (test_radio.py builds it with ASan/UBSan): which radio a USB stick
// lands on as sticks come and go behind a hub, when a radio shows or frees itself, a new radio's settings, the
// per-radio settings checks, the 1.x migration and the full-speed re-force rule.
//
//   radio_harness selftest

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "radio_core.h"

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

static radio_ident_t stick(uint16_t vid, uint16_t pid, const char *serial, uint8_t port) {
    radio_ident_t id = {.vid = vid, .pid = pid, .port = port};
    snprintf(id.serial, sizeof(id.serial), "%s", serial);
    return id;
}

// The sticks of the house: two SONOFF MG24s share 10c4:ea60 (Zigbee + Thread), a Z-Wave stick, a CH340 with
// no serial.
#define MG24 0x10c4, 0xea60
#define ZWAVE 0x1a86, 0x55d4
#define CH340 0x1a86, 0x7523

typedef struct {
    radio_slot_t slots[RADIO_MAX];
    bool in_use[RADIO_MAX];
} bridge_t;

static void fresh(bridge_t *b) { memset(b, 0, sizeof(*b)); }

// What dongle.c does: assign, save the binding for a new or replaced stick (and a moved port), mark in use.
static int plug(bridge_t *b, radio_ident_t dev, radio_assign_t *how) {
    radio_assign_t r;
    int i = radio_assign(b->slots, b->in_use, &dev, &r);
    if (how) *how = r;
    if (i == RADIO_NONE) return i;
    if (r == RA_NEW || r == RA_REPLACED || (r == RA_BOUND && b->slots[i].bound.port != dev.port))
        b->slots[i].bound = dev;
    b->in_use[i] = true;
    return i;
}

static void unplug(bridge_t *b, int i) { b->in_use[i] = false; }

static void t_direct_on_j3(void) {
    bridge_t b;
    fresh(&b);
    radio_assign_t how;
    CHECK(plug(&b, stick(MG24, "Z1", 0), &how) == 0 && how == RA_REPLACED, "first stick on J3 -> radio 0, saved");
    unplug(&b, 0);
    CHECK(plug(&b, stick(MG24, "Z1", 0), &how) == 0 && how == RA_BOUND, "same stick again -> bound");
    unplug(&b, 0);
    // swapping the dongle on J3 keeps working as before multi-radio: whatever is on J3 is radio 0
    CHECK(plug(&b, stick(CH340, "", 0), &how) == 0 && how == RA_REPLACED, "another stick on J3 replaces radio 0's");
    CHECK(b.slots[0].bound.vid == 0x1a86, "radio 0 now saved as the CH340");
    // a second J3 device can't happen, but must not land anywhere
    CHECK(plug(&b, stick(MG24, "Z1", 0), &how) == RADIO_NONE && how == RA_FULL, "J3 busy -> refused");
}

static void t_moved_behind_hub(void) {
    bridge_t b;
    fresh(&b);
    plug(&b, stick(MG24, "Z1", 0), NULL);  // the setup today: Zigbee stick on J3
    unplug(&b, 0);
    radio_assign_t how;
    // a hub goes in: the Thread stick happens to enumerate first
    int t = plug(&b, stick(MG24, "T1", 2), &how);
    CHECK(t == 1 && how == RA_NEW, "unknown Thread stick must not take Zigbee's radio 0 (got %d)", t);
    int z = plug(&b, stick(MG24, "Z1", 1), &how);
    CHECK(z == 0 && how == RA_BOUND, "Zigbee stick finds radio 0 by serial behind the hub (got %d)", z);
    CHECK(b.slots[0].bound.port == 1, "radio 0's port follows the stick (%u)", b.slots[0].bound.port);
    int w = plug(&b, stick(ZWAVE, "W1", 3), &how);
    CHECK(w == 2 && how == RA_NEW, "Z-Wave stick -> radio 2 (got %d)", w);
    int z2 = plug(&b, stick(MG24, "H1", 4), &how);
    CHECK(z2 == 3 && how == RA_NEW, "a 4th stick (ZHA) -> radio 3 (got %d)", z2);
    radio_assign_t h5;
    CHECK(plug(&b, stick(CH340, "", 5), &h5) == RADIO_NONE && h5 == RA_FULL, "5th stick refused");
}

static void t_boot_order_independent(void) {
    // Sticks bound once; at every boot they enumerate in a different order and must land in the same radios.
    static const int orders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    radio_ident_t s[3] = {stick(MG24, "Z1", 1), stick(MG24, "T1", 2), stick(ZWAVE, "W1", 3)};  // radio 3 stays free
    bridge_t b;
    fresh(&b);
    for (int k = 0; k < 3; k++) b.slots[k].bound = s[k];
    for (int o = 0; o < 6; o++) {
        memset(b.in_use, 0, sizeof(b.in_use));
        for (int k = 0; k < 3; k++) {
            int which = orders[o][k];
            radio_assign_t how;
            int i = plug(&b, s[which], &how);
            CHECK(i == which && how == RA_BOUND, "order %d: stick %d -> radio %d", o, which, i);
        }
    }
}

static void t_replug_and_port_moves(void) {
    bridge_t b;
    fresh(&b);
    plug(&b, stick(MG24, "Z1", 1), NULL);
    plug(&b, stick(MG24, "T1", 2), NULL);
    unplug(&b, 1);
    radio_assign_t how;
    // the Thread stick comes back on another hub port: serial still finds radio 1
    CHECK(plug(&b, stick(MG24, "T1", 4), &how) == 1 && how == RA_BOUND, "replug on another port -> same radio");
    CHECK(b.slots[1].bound.port == 4, "saved port updated");
}

static void t_no_serial_sticks(void) {
    // Two CH340s without serial numbers: told apart by hub port only.
    bridge_t b;
    fresh(&b);
    radio_assign_t how;
    CHECK(plug(&b, stick(CH340, "", 1), &how) == 0 && how == RA_NEW, "CH340 on port 1 -> radio 0");
    CHECK(plug(&b, stick(CH340, "", 2), &how) == 1 && how == RA_NEW, "CH340 on port 2 -> radio 1");
    memset(b.in_use, 0, sizeof(b.in_use));
    CHECK(plug(&b, stick(CH340, "", 2), &how) == 1 && how == RA_BOUND, "port 2 first after reboot -> radio 1");
    CHECK(plug(&b, stick(CH340, "", 1), &how) == 0 && how == RA_BOUND, "port 1 -> radio 0");
    memset(b.in_use, 0, sizeof(b.in_use));
    // a serial-less stick on a port nobody saved is a new stick
    CHECK(plug(&b, stick(CH340, "", 3), &how) == 2 && how == RA_NEW, "CH340 on new port 3 -> free radio 2");
}

static void t_duplicate_serials(void) {
    // Two clones flashed with the same serial, saved on ports 1 and 2. One comes back on a port nobody saved: it
    // takes the first free radio saved with that serial; the other then finds the remaining one by serial.
    bridge_t b;
    fresh(&b);
    b.slots[0].bound = stick(MG24, "0001", 1);
    b.slots[1].bound = stick(MG24, "0001", 2);
    radio_assign_t how;
    CHECK(plug(&b, stick(MG24, "0001", 2), &how) == 1 && how == RA_BOUND, "clone on port 2 -> radio 2 (its port)");
    int i = plug(&b, stick(MG24, "0001", 3), &how);
    CHECK(i == 0, "clone on an unsaved port -> the free radio with its serial (got %d)", i);
    radio_assign_t h3;
    CHECK(plug(&b, stick(MG24, "0001", 4), &h3) == 2 && h3 == RA_NEW, "a third clone is a new stick");
}

static void t_duplicate_serials_swapped(void) {
    // Clones with one serial on ports 1 and 2, saved that way: whichever enumerates first, each finds its own radio.
    for (int order = 0; order < 2; order++) {
        bridge_t b;
        fresh(&b);
        b.slots[0].bound = stick(MG24, "0001", 1);
        b.slots[1].bound = stick(MG24, "0001", 2);
        radio_assign_t how;
        uint8_t first = order ? 2 : 1, second = order ? 1 : 2;
        int a = plug(&b, stick(MG24, "0001", first), &how);
        int c = plug(&b, stick(MG24, "0001", second), &how);
        CHECK(a == first - 1 && c == second - 1, "order %d: port %u -> radio %d, port %u -> radio %d", order, first, a, second, c);
        CHECK(b.slots[0].bound.port == 1 && b.slots[1].bound.port == 2, "order %d: no swap saved", order);
    }
}

static void t_j3_serialless_moved(void) {
    // A CH340 without a serial lived on J3 (saved with port 0); a hub goes in and it lands on port 3: still radio 1.
    bridge_t b;
    fresh(&b);
    plug(&b, stick(CH340, "", 0), NULL);
    unplug(&b, 0);
    radio_assign_t how;
    int i = plug(&b, stick(CH340, "", 3), &how);
    CHECK(i == 0 && how == RA_BOUND, "serial-less J3 stick behind the hub -> radio 1 (got %d)", i);
    // but only radio 0 gets that leniency: a serial-less stick saved on hub port 2 stays a port-2 stick
    bridge_t c;
    fresh(&c);
    c.slots[1].bound = stick(CH340, "", 2);
    c.slots[0].bound = stick(MG24, "Z1", 1);
    CHECK(plug(&c, stick(CH340, "", 4), &how) == 2 && how == RA_NEW, "radio 2's port-2 stick on port 4 is a new stick");
}

static void t_lowest_free(void) {
    // Radio 1 was freed (its stick left before anything was set up on it): the next new stick takes radio 1, not 3.
    bridge_t b;
    fresh(&b);
    plug(&b, stick(MG24, "Z1", 1), NULL);
    plug(&b, stick(MG24, "T1", 2), NULL);
    plug(&b, stick(ZWAVE, "W1", 3), NULL);
    unplug(&b, 1);
    memset(&b.slots[1].bound, 0, sizeof(b.slots[1].bound));  // what dongle.c does when radio_unplug_frees says so
    radio_assign_t how;
    CHECK(plug(&b, stick(MG24, "H1", 4), &how) == 1 && how == RA_NEW, "new stick -> the lowest free radio (1)");
    CHECK(plug(&b, stick(MG24, "T1", 2), &how) == 3 && how == RA_NEW, "the freed stick is new now -> radio 3");
}

static void t_forget(void) {
    // Replacing a dead Zigbee stick behind the hub: unknown sticks never take a saved radio, so "Forget" it first.
    bridge_t b;
    fresh(&b);
    plug(&b, stick(MG24, "Z1", 1), NULL);
    plug(&b, stick(MG24, "T1", 2), NULL);
    plug(&b, stick(ZWAVE, "W1", 3), NULL);
    plug(&b, stick(MG24, "H1", 4), NULL);
    unplug(&b, 0);
    radio_assign_t how;
    CHECK(plug(&b, stick(MG24, "Z2", 1), &how) == RADIO_NONE && how == RA_FULL, "replacement refused while radio 0 saved");
    memset(&b.slots[0].bound, 0, sizeof(b.slots[0].bound));  // Forget
    CHECK(plug(&b, stick(MG24, "Z2", 1), &how) == 0 && how == RA_NEW, "after Forget the replacement takes radio 0");
}

static void t_ident(void) {
    radio_ident_t a = stick(MG24, "Z1", 1), b2 = stick(MG24, "Z1", 3), c = stick(MG24, "Z2", 1), e = {0};
    CHECK(radio_ident_same(&a, &b2), "same serial, other port: same stick");
    CHECK(!radio_ident_same(&a, &c), "other serial: other stick");
    CHECK(!radio_ident_same(&a, &e) && !radio_ident_same(&e, &e), "empty is nobody");
    radio_ident_t n1 = stick(CH340, "", 1), n2 = stick(CH340, "", 2);
    CHECK(!radio_ident_same(&n1, &n2) && radio_ident_same(&n1, &n1), "no serial: port decides");
    radio_assign_t how;
    bridge_t b;
    fresh(&b);
    CHECK(radio_assign(b.slots, b.in_use, &e, &how) == RADIO_NONE, "empty ident refused");
}

// All radios shown (each has a stick or is set up), with the ports they would get.
static void defaults_all(radio_link_t l[RADIO_MAX]) {
    for (int i = 0; i < RADIO_MAX; i++) radio_link_default(i, &l[i]);
}
#define ALL ((1u << RADIO_MAX) - 1)

static void t_links(void) {
    radio_link_t l[RADIO_MAX];
    char err[128];
    defaults_all(l);
    CHECK(radio_check_links(l, 1u, err, sizeof(err)), "fresh bridge (radio 1 only) passes: %s", err);
    CHECK(radio_check_links(l, ALL, err, sizeof(err)), "all four radios pass: %s", err);
    CHECK(l[0].tcp_port == 6638 && l[1].tcp_port == 6639 && l[2].tcp_port == 6640 && l[3].tcp_port == 6641, "default ports");
    CHECK(l[0].role == RADIO_ROLE_ZIGBEE && l[1].role == RADIO_ROLE_ZHA && l[3].role == RADIO_ROLE_ZHA, "default roles");
    l[0].tcp_port = 6639;  // radio 1 moved onto radio 2's default port: fine while radio 2 isn't shown
    CHECK(radio_check_links(l, 1u, err, sizeof(err)), "radio 1 on 6639 with radio 2 hidden: %s", err);
    CHECK(!radio_check_links(l, 0x3u, err, sizeof(err)) && strstr(err, "Radio 2") && strstr(err, "radio 1"),
          "clash once radio 2 shows: %s", err);
    CHECK(!radio_check_links(l, 0x2u, err, sizeof(err)), "radio 1 counts as shown even if the mask forgets it");

    defaults_all(l);
    l[3].tcp_port = 6640;
    CHECK(!radio_check_links(l, ALL, err, sizeof(err)) && strstr(err, "Radio 4") && strstr(err, "radio 3"), "duplicate port: %s", err);
    CHECK(radio_check_links(l, 0x7u, err, sizeof(err)), "a hidden radio may keep a clashing port: %s", err);

    defaults_all(l);
    l[1].tcp_port = 80;
    CHECK(!radio_check_links(l, ALL, err, sizeof(err)) && strstr(err, "Radio 2"), "port 80: %s", err);
    defaults_all(l);
    l[1].tcp_port = 0;
    CHECK(!radio_check_links(l, ALL, err, sizeof(err)), "port 0");
    defaults_all(l);
    l[2].baud = 1200;
    CHECK(!radio_check_links(l, ALL, err, sizeof(err)), "baud too low");
    l[2].baud = 1000000;
    CHECK(!radio_check_links(l, ALL, err, sizeof(err)), "baud too high");
    l[2].baud = 921600;
    CHECK(radio_check_links(l, ALL, err, sizeof(err)), "921600 ok");
    defaults_all(l);
    l[1].role = 4;
    CHECK(!radio_check_links(l, ALL, err, sizeof(err)), "bad role");
    l[1].role = 4;
    CHECK(!radio_check_links(l, 1u, err, sizeof(err)), "bad role refused on a hidden radio too");
    l[1].role = RADIO_ROLE_ZHA;
    l[2].role = RADIO_ROLE_THREAD;
    l[3].role = RADIO_ROLE_ZWAVE;
    CHECK(radio_check_links(l, ALL, err, sizeof(err)), "Z2M, ZHA, Thread, Z-Wave: %s", err);
    l[0].role = RADIO_ROLE_ZHA;
    CHECK(!radio_check_links(l, ALL, err, sizeof(err)) && strstr(err, "Radio 1") != NULL, "ZHA refused on radio 1");
    defaults_all(l);
    strcpy(l[1].name, "Thread <b>");
    CHECK(!radio_check_links(l, ALL, err, sizeof(err)), "markup in a name");
    strcpy(l[1].name, "Thread (MG24)");
    CHECK(radio_check_links(l, ALL, err, sizeof(err)), "plain name ok: %s", err);
    memset(l[1].name, 'a', sizeof(l[1].name));  // no terminator
    CHECK(!radio_check_links(l, ALL, err, sizeof(err)), "unterminated name refused");
    defaults_all(l);
    strcpy(l[2].allow, "192.168.1.5");
    CHECK(radio_check_links(l, ALL, err, sizeof(err)), "host ok");
    strcpy(l[2].allow, "192.168.1.0/24");
    CHECK(radio_check_links(l, ALL, err, sizeof(err)), "subnet ok");
    strcpy(l[2].allow, "192.168.1.5x");
    CHECK(!radio_check_links(l, ALL, err, sizeof(err)), "trailing junk refused");
    strcpy(l[2].allow, "192.168.1.256");
    CHECK(!radio_check_links(l, ALL, err, sizeof(err)), "octet > 255 refused");
    CHECK(radio_check_links(l, 0x3u, err, sizeof(err)), "hidden radio's host not checked");
}

static void t_fresh_link(void) {
    radio_link_t l[RADIO_MAX], n;
    defaults_all(l);
    strcpy(l[0].allow, "192.168.1.50");
    CHECK(radio_guess_role(0, RADIO_KIND_ZIGBEE) == RADIO_ROLE_ZIGBEE, "Zigbee on radio 1 -> Zigbee2MQTT");
    CHECK(radio_guess_role(0, RADIO_KIND_UNKNOWN) == RADIO_ROLE_ZIGBEE, "unknown on radio 1 -> Zigbee2MQTT");
    CHECK(radio_guess_role(1, RADIO_KIND_ZIGBEE) == RADIO_ROLE_ZHA, "Zigbee on radio 2 -> ZHA");
    CHECK(radio_guess_role(2, RADIO_KIND_UNKNOWN) == RADIO_ROLE_ZHA, "unknown on radio 3 -> ZHA");
    CHECK(radio_guess_role(1, RADIO_KIND_THREAD) == RADIO_ROLE_THREAD && radio_guess_role(0, RADIO_KIND_THREAD) == RADIO_ROLE_THREAD,
          "Thread stick -> Thread anywhere");
    CHECK(radio_guess_role(3, RADIO_KIND_ZWAVE) == RADIO_ROLE_ZWAVE, "Z-Wave stick -> Z-Wave");

    radio_fresh_link(2, RADIO_KIND_THREAD, 0, l, 0x3u, &n);
    CHECK(n.role == RADIO_ROLE_THREAD && n.baud == 460800, "Thread: RCP baud 460800 (got %lu)", (unsigned long)n.baud);
    CHECK(strcmp(n.allow, "192.168.1.50") == 0, "allowed host copied from radio 1 (got '%s')", n.allow);
    CHECK(n.tcp_port == 6640 && n.name[0] == 0, "own default port, no name");
    radio_fresh_link(1, RADIO_KIND_ZIGBEE, 115200, l, 0x1u, &n);
    CHECK(n.role == RADIO_ROLE_ZHA && n.baud == 115200 && n.tcp_port == 6639, "ZNP stick on radio 2: ZHA, 115200, 6639");
    radio_fresh_link(3, RADIO_KIND_ZWAVE, 0, l, 0x7u, &n);
    CHECK(n.baud == 115200 && n.tcp_port == 6641, "Z-Wave: 115200 on 6641");

    // someone moved radio 1 to 6639: radio 2 must not get 6639 too
    l[0].tcp_port = 6639;
    radio_fresh_link(1, RADIO_KIND_ZIGBEE, 0, l, 0x1u, &n);
    CHECK(n.tcp_port != 6639 && n.tcp_port != 80, "port clash avoided (got %u)", n.tcp_port);
    CHECK(n.tcp_port == 6638, "lowest free port is 6638 (got %u)", n.tcp_port);
    l[1].tcp_port = 6640;  // radio 2 (shown) sits on radio 3's default
    CHECK(radio_pick_port(l, 0x3u, 2) == 6638, "radio 3 skips 6640 (radio 2) and 6639 (radio 1)");
    CHECK(radio_pick_port(l, 0x1u, 2) == 6640, "a hidden radio's port doesn't block");
}

static void t_lifecycle(void) {
    // shown
    CHECK(radio_shown(0, false, false), "radio 1 always shown");
    CHECK(!radio_shown(1, false, false), "no stick, not set up: hidden");
    CHECK(radio_shown(1, false, true), "stick there: shown");
    CHECK(radio_shown(2, true, false), "set up, stick gone: shown (with an error)");
    // state
    CHECK(radio_state(0, false, false, false, false) == RS_NO_STICK, "radio 1 without stick");
    CHECK(radio_state(1, false, false, false, false) == RS_HIDDEN, "radio 2 without stick: hidden");
    CHECK(radio_state(1, true, false, false, false) == RS_MISSING, "set up, gone: missing");
    CHECK(radio_state(0, true, false, false, false) == RS_MISSING, "radio 1 set up, gone: missing");
    CHECK(radio_state(1, false, true, false, false) == RS_OPENING, "present, not open");
    CHECK(radio_state(1, true, true, true, false) == RS_READY, "open");
    CHECK(radio_state(1, true, false, false, true) == RS_FLASHING, "in its bootloader: flashing, not missing");
    CHECK(strcmp(radio_state_key(RS_MISSING), "missing") == 0 && strcmp(radio_state_key(RS_READY), "ready") == 0 &&
              strcmp(radio_state_key(RS_HIDDEN), "hidden") == 0 && strcmp(radio_state_key(RS_NO_STICK), "no_stick") == 0 &&
              strcmp(radio_state_key(RS_OPENING), "opening") == 0 && strcmp(radio_state_key(RS_FLASHING), "flashing") == 0,
          "state keys");
    // unplug
    CHECK(radio_unplug_frees(false, false), "never used: freed, unplugging harms nothing");
    CHECK(!radio_unplug_frees(true, false), "set up: kept, waits for its stick");
    CHECK(!radio_unplug_frees(false, true), "being flashed (bootloader re-enumerates): kept");
    // stale bindings after boot
    bool bound[RADIO_MAX] = {true, true, true, false}, su[RADIO_MAX] = {true, false, false, false},
         pr[RADIO_MAX] = {false, true, false, false};
    CHECK(radio_stale_mask(bound, su, pr) == 0x4u, "only radio 3 (bound, not set up, absent) is freed (got %#x)",
          (unsigned)radio_stale_mask(bound, su, pr));
}

static void t_diff_ports(void) {
    radio_link_t a, b;
    radio_link_default(1, &a);
    b = a;
    CHECK(radio_link_diff(&a, &b) == 0, "same");
    b.tcp_port = 7000;
    b.baud = 460800;
    CHECK(radio_link_diff(&a, &b) == (RL_PORT | RL_BAUD), "port + baud");
    b = a;
    strcpy(b.allow, "10.0.0.1");
    b.role = RADIO_ROLE_THREAD;
    strcpy(b.name, "T");
    CHECK(radio_link_diff(&a, &b) == (RL_ALLOW | RL_ROLE | RL_NAME), "allow + role + name");

    radio_link_t l[RADIO_MAX];
    defaults_all(l);
    char buf[32];
    CHECK(radio_ports_txt(l, 0x5u, buf, sizeof(buf)) && strcmp(buf, "6638,6640") == 0, "shown ports (%s)", buf);
    CHECK(radio_ports_txt(l, 0u, buf, sizeof(buf)) && strcmp(buf, "6638") == 0, "radio 1 always (%s)", buf);
    for (int i = 0; i < RADIO_MAX; i++) l[i].tcp_port = (uint16_t)(65535 - i);
    CHECK(radio_ports_txt(l, ALL, buf, sizeof(buf)), "four 5-digit ports fit in 32 bytes (%s)", buf);
    char small[12];
    CHECK(!radio_ports_txt(l, ALL, small, sizeof(small)) && strlen(small) < sizeof(small), "too small: refused, terminated");
}

static void t_migrate(void) {
    // 1.x: radio 2 (Thread, off, never used) kept 6639; radio 1 had been moved to 6639 too; radio 3 had a Z-Wave
    // stick saved on 6640. 2.0: radios with a saved stick are set up; a set-up radio clashing on a port moves.
    radio_link_t l[RADIO_MAX];
    defaults_all(l);
    l[0].tcp_port = 6639;
    bool bound[RADIO_MAX] = {true, false, true, false}, su[RADIO_MAX], on[RADIO_MAX] = {true, false, true, false};
    uint32_t moved = radio_migrate_v2(l, bound, on, su);
    CHECK(su[0] && !su[1] && su[2] && !su[3], "set up = had a saved stick");
    CHECK(moved == 0 && l[0].tcp_port == 6639 && l[2].tcp_port == 6640, "no clash among shown radios: nothing moves");
    defaults_all(l);
    l[2].tcp_port = 6638;  // radio 3 on radio 1's port (it was off in 1.x, so that was allowed)
    bool b2[RADIO_MAX] = {true, false, true, true}, on2[RADIO_MAX] = {true, false, false, true};
    moved = radio_migrate_v2(l, b2, on2, su);
    char err[96];
    CHECK((moved & 0x4u) && l[2].tcp_port != 6638, "radio 3 moved off 6638 (moved %#x, port %u)", (unsigned)moved, l[2].tcp_port);
    CHECK(radio_check_links(l, 0xDu, err, sizeof(err)), "after migration the shown radios pass: %s", err);

    // Radio 2 was off but had a stick, on 6639; radio 3 was on and in use on 6639 too (allowed while 2 was off).
    // The radio that was on keeps its port (its client uses it); the off one moves, and gets radio 1's host.
    defaults_all(l);
    strcpy(l[0].allow, "192.168.1.50");
    l[1].tcp_port = 6639;
    l[2].tcp_port = 6639;
    strcpy(l[2].allow, "192.168.1.40");
    bool b3[RADIO_MAX] = {true, true, true, false}, on3[RADIO_MAX] = {true, false, true, false};
    moved = radio_migrate_v2(l, b3, on3, su);
    CHECK(l[2].tcp_port == 6639, "the radio that was on keeps 6639 (got %u)", l[2].tcp_port);
    CHECK(l[1].tcp_port != 6639 && (moved & 0x2u), "the off radio moves (got %u)", l[1].tcp_port);
    CHECK(strcmp(l[1].allow, "192.168.1.50") == 0, "the off radio listens for radio 1's host, not anyone ('%s')", l[1].allow);
    CHECK(strcmp(l[2].allow, "192.168.1.40") == 0 && !(moved & 0x4u), "the on radio is untouched");
    CHECK(radio_check_links(l, 0x7u, err, sizeof(err)), "and they pass: %s", err);
}

static void t_parse_allow(void) {
    uint32_t a, m;
    CHECK(radio_parse_allow("", &a, &m) && a == 0 && m == 0, "empty = anyone");
    CHECK(radio_parse_allow("192.168.1.50", &a, &m) && a == 0xC0A80132u && m == 0xFFFFFFFFu, "host");
    CHECK(radio_parse_allow("192.168.1.77/24", &a, &m) && a == 0xC0A80100u && m == 0xFFFFFF00u, "subnet masks the host part");
    CHECK(radio_parse_allow("10.0.0.0/0", &a, &m) && a == 0 && m == 0, "/0 = anyone");
    CHECK(!radio_parse_allow("1.2.3.4/", &a, &m), "slash without bits");
    CHECK(!radio_parse_allow("1.2.3.4/33", &a, &m), "/33");
    CHECK(!radio_parse_allow("1.2.3", &a, &m), "3 octets");
    CHECK(!radio_parse_allow("1.2.3.4/24x", &a, &m), "junk after bits");
    CHECK(!radio_parse_allow("1.2.3.4 ", &a, &m), "trailing space");
}

static void t_reforce(void) {
    int64_t last = 0;
    const int64_t s = 1000000;
    CHECK(!radio_hub_reforce(false, true, true, 10 * s, &last), "fs-only off: never");
    CHECK(!radio_hub_reforce(true, false, true, 10 * s, &last), "not a hub: never");
    CHECK(!radio_hub_reforce(true, true, false, 10 * s, &last), "hub at full speed: fine");
    CHECK(radio_hub_reforce(true, true, true, 10 * s, &last), "hub at high speed: re-force");
    CHECK(!radio_hub_reforce(true, true, true, 30 * s, &last), "again within a minute: not yet");
    CHECK(radio_hub_reforce(true, true, true, 71 * s, &last), "a minute later: again");
    int64_t l0 = 0;
    CHECK(radio_hub_reforce(true, true, true, 0, &l0) && l0 != 0, "at t=0 still remembered");
    CHECK(!radio_hub_reforce(true, true, true, 5 * s, &l0), "and limited");
}

static void t_names(void) {
    CHECK(strcmp(radio_role_name(RADIO_ROLE_ZIGBEE), "Zigbee") == 0 && strcmp(radio_role_name(RADIO_ROLE_THREAD), "Thread") == 0 &&
              strcmp(radio_role_name(RADIO_ROLE_ZWAVE), "Z-Wave") == 0 && strcmp(radio_role_name(RADIO_ROLE_ZHA), "ZHA") == 0 &&
              strcmp(radio_role_name(9), "?") == 0,
          "role names");
}

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "selftest") != 0) {
        fprintf(stderr, "usage: radio_harness selftest\n");
        return 2;
    }
    t_direct_on_j3();
    t_moved_behind_hub();
    t_boot_order_independent();
    t_replug_and_port_moves();
    t_no_serial_sticks();
    t_duplicate_serials();
    t_duplicate_serials_swapped();
    t_j3_serialless_moved();
    t_lowest_free();
    t_forget();
    t_ident();
    t_links();
    t_fresh_link();
    t_lifecycle();
    t_diff_ports();
    t_migrate();
    t_parse_allow();
    t_reforce();
    t_names();
    printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "all passed", g_fail);
    return g_fail ? 1 : 0;
}
