#include "radio_core.h"

#include <stdio.h>
#include <string.h>

bool radio_ident_empty(const radio_ident_t *id) { return id->vid == 0 && id->pid == 0; }

static bool same_model(const radio_ident_t *a, const radio_ident_t *b) { return a->vid == b->vid && a->pid == b->pid; }

bool radio_ident_same(const radio_ident_t *a, const radio_ident_t *b) {
    if (radio_ident_empty(a) || radio_ident_empty(b) || !same_model(a, b)) return false;
    if (a->serial[0] && b->serial[0]) return strcmp(a->serial, b->serial) == 0;
    return a->port == b->port;
}

int radio_assign(const radio_slot_t slots[RADIO_MAX], const bool in_use[RADIO_MAX], const radio_ident_t *dev,
                 radio_assign_t *result) {
    *result = RA_FULL;
    if (radio_ident_empty(dev)) return RADIO_NONE;

    // 1. Straight on J3: nothing else can be plugged in, so this is radio 0's stick whatever it is.
    if (dev->port == 0) {
        if (in_use[0]) return RADIO_NONE;
        *result = radio_ident_same(&slots[0].bound, dev) ? RA_BOUND : RA_REPLACED;
        return 0;
    }

    // 2a. The radio saved for this stick by serial. Two sticks sharing a serial (clones): the hub port decides
    // between the free radios saved with it, so they can't swap radios when they enumerate the other way round.
    bool serial_clash = false;
    if (dev->serial[0]) {
        int first = RADIO_NONE, same_port = RADIO_NONE, matches = 0;
        for (int i = 0; i < RADIO_MAX; i++) {
            const radio_ident_t *b = &slots[i].bound;
            if (radio_ident_empty(b) || !same_model(b, dev) || strcmp(b->serial, dev->serial) != 0) continue;
            matches++;
            if (in_use[i]) {
                serial_clash = true;  // two sticks with one serial: tell them apart by hub port below
                continue;
            }
            if (first == RADIO_NONE) first = i;
            if (same_port == RADIO_NONE && b->port == dev->port) same_port = i;
        }
        int pick = matches > 1 ? (same_port != RADIO_NONE ? same_port : first) : first;
        if (pick != RADIO_NONE) {
            *result = RA_BOUND;
            return pick;
        }
    }
    // 2b. By hub port, for sticks without a (usable) serial.
    for (int i = 0; i < RADIO_MAX; i++) {
        const radio_ident_t *b = &slots[i].bound;
        if (in_use[i] || radio_ident_empty(b) || !same_model(b, dev)) continue;
        // Radio 0's serial-less stick saved straight on J3 is found on any hub port (the hub went in later).
        if (b->port != dev->port && !(i == 0 && b->port == 0 && !b->serial[0])) continue;
        if (b->serial[0] && dev->serial[0] && !serial_clash) continue;  // has its own serial: matched above or not ours
        *result = RA_BOUND;
        return i;
    }
    // 3. A new stick takes the lowest free radio (none saved).
    for (int i = 0; i < RADIO_MAX; i++) {
        if (in_use[i] || !radio_ident_empty(&slots[i].bound)) continue;
        *result = RA_NEW;
        return i;
    }
    return RADIO_NONE;
}

const char *radio_role_name(uint8_t role) {
    switch (role) {
        case RADIO_ROLE_ZIGBEE: return "Zigbee";
        case RADIO_ROLE_THREAD: return "Thread";
        case RADIO_ROLE_ZWAVE: return "Z-Wave";
        case RADIO_ROLE_ZHA: return "ZHA";
        default: return "?";
    }
}

void radio_link_default(int i, radio_link_t *out) {
    memset(out, 0, sizeof(*out));
    out->tcp_port = (uint16_t)(RADIO_PORT_BASE + i);
    out->role = i == 0 ? RADIO_ROLE_ZIGBEE : RADIO_ROLE_ZHA;
    out->baud = 115200;
}

uint8_t radio_guess_role(int i, radio_kind_t kind) {
    switch (kind) {
        case RADIO_KIND_THREAD: return RADIO_ROLE_THREAD;
        case RADIO_KIND_ZWAVE: return RADIO_ROLE_ZWAVE;
        default: return i == 0 ? RADIO_ROLE_ZIGBEE : RADIO_ROLE_ZHA;
    }
}

static bool port_taken(const radio_link_t links[RADIO_MAX], uint32_t shown_mask, int self, uint16_t port) {
    for (int j = 0; j < RADIO_MAX; j++)
        if (j != self && ((shown_mask >> j) & 1u) && links[j].tcp_port == port) return true;
    return false;
}

uint16_t radio_pick_port(const radio_link_t links[RADIO_MAX], uint32_t shown_mask, int i) {
    shown_mask |= 1u;  // radio 0 always listens
    uint16_t want = (uint16_t)(RADIO_PORT_BASE + i);
    if (!port_taken(links, shown_mask, i, want)) return want;
    for (uint32_t p = RADIO_PORT_BASE; p <= 65535; p++)
        if (p != 80 && !port_taken(links, shown_mask, i, (uint16_t)p)) return (uint16_t)p;
    return want;  // can't happen with RADIO_MAX radios
}

void radio_fresh_link(int i, radio_kind_t kind, uint32_t model_baud, const radio_link_t links[RADIO_MAX],
                      uint32_t shown_mask, radio_link_t *out) {
    radio_link_t l;
    memset(&l, 0, sizeof(l));
    l.role = radio_guess_role(i, kind);
    l.baud = model_baud ? model_baud : l.role == RADIO_ROLE_THREAD ? 460800 : 115200;
    memcpy(l.allow, links[0].allow, sizeof(l.allow));  // the same Home Assistant as radio 0, not "anyone"
    l.allow[sizeof(l.allow) - 1] = 0;
    l.tcp_port = radio_pick_port(links, shown_mask, i);
    *out = l;
}

bool radio_shown(int i, bool set_up, bool present) { return i == 0 || present || set_up; }

radio_state_t radio_state(int i, bool set_up, bool present, bool open, bool flashing) {
    if (flashing) return RS_FLASHING;
    if (present) return open ? RS_READY : RS_OPENING;
    if (set_up) return RS_MISSING;
    return i == 0 ? RS_NO_STICK : RS_HIDDEN;
}

const char *radio_state_key(radio_state_t s) {
    switch (s) {
        case RS_NO_STICK: return "no_stick";
        case RS_MISSING: return "missing";
        case RS_OPENING: return "opening";
        case RS_READY: return "ready";
        case RS_FLASHING: return "flashing";
        default: return "hidden";
    }
}

bool radio_unplug_frees(bool set_up, bool flashing) { return !set_up && !flashing; }

uint32_t radio_stale_mask(const bool bound[RADIO_MAX], const bool set_up[RADIO_MAX], const bool present[RADIO_MAX]) {
    uint32_t m = 0;
    for (int i = 0; i < RADIO_MAX; i++)
        if (bound[i] && !set_up[i] && !present[i]) m |= 1u << i;
    return m;
}

unsigned radio_link_diff(const radio_link_t *a, const radio_link_t *b) {
    unsigned d = 0;
    if (a->tcp_port != b->tcp_port) d |= RL_PORT;
    if (a->baud != b->baud) d |= RL_BAUD;
    if (strncmp(a->allow, b->allow, sizeof(a->allow)) != 0) d |= RL_ALLOW;
    if (a->role != b->role) d |= RL_ROLE;
    if (strncmp(a->name, b->name, sizeof(a->name)) != 0) d |= RL_NAME;
    return d;
}

bool radio_ports_txt(const radio_link_t links[RADIO_MAX], uint32_t shown_mask, char *buf, size_t n) {
    if (n == 0) return false;
    buf[0] = 0;
    size_t len = 0;
    shown_mask |= 1u;
    for (int i = 0; i < RADIO_MAX; i++) {
        if (!((shown_mask >> i) & 1u)) continue;
        int w = snprintf(buf + len, n - len, "%s%u", len ? "," : "", links[i].tcp_port);
        if (w < 0 || (size_t)w >= n - len) {
            buf[len] = 0;
            return false;
        }
        len += (size_t)w;
    }
    return true;
}

uint32_t radio_migrate_v2(radio_link_t links[RADIO_MAX], const bool bound[RADIO_MAX], const bool was_on[RADIO_MAX],
                          bool set_up_out[RADIO_MAX]) {
    uint32_t shown = 1u, moved = 0;
    for (int i = 0; i < RADIO_MAX; i++) {
        set_up_out[i] = bound[i];
        if (bound[i]) shown |= 1u << i;
    }
    // Two passes in radio order: first the radios that were on (their ports were unique among the on radios and
    // their clients use them), then the ones that were off. Each moves only if an earlier settled radio has its port.
    // Hidden radios keep theirs; a radio that gets set up later picks a free port then.
    uint32_t done = 1u;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 1; i < RADIO_MAX; i++) {
            if (!((shown >> i) & 1u) || (was_on[i] ? pass != 0 : pass != 1)) continue;
            if (links[i].tcp_port < 1 || links[i].tcp_port == 80 || port_taken(links, done, i, links[i].tcp_port)) {
                links[i].tcp_port = radio_pick_port(links, done, i);
                moved |= 1u << i;
            }
            if (!was_on[i] && links[i].allow[0] == 0 && links[0].allow[0] != 0) {
                memcpy(links[i].allow, links[0].allow, sizeof(links[i].allow));  // not "anyone" now it listens
                moved |= 1u << i;
            }
            done |= 1u << i;
        }
    }
    return moved;
}

bool radio_parse_allow(const char *s, uint32_t *addr_host, uint32_t *mask_host) {
    if (s == NULL || s[0] == '\0') {
        *addr_host = 0;
        *mask_host = 0;
        return true;
    }
    unsigned a, b, c, d, n = 32;
    char tail;
    int got = sscanf(s, "%u.%u.%u.%u/%u%c", &a, &b, &c, &d, &n, &tail);
    if (got == 4 && strchr(s, '/') != NULL) return false;
    if ((got != 4 && got != 5) || a > 255 || b > 255 || c > 255 || d > 255 || n > 32) return false;
    // sscanf stops at the first non-digit after d: "1.2.3.4x" or "1.2.3.4 5" must not pass as 1.2.3.4
    if (got == 4) {
        char back[16];
        snprintf(back, sizeof(back), "%u.%u.%u.%u", a, b, c, d);
        if (strlen(s) != strlen(back)) return false;
    }
    uint32_t m = n == 0 ? 0 : 0xFFFFFFFFu << (32 - n);
    *addr_host = ((a << 24) | (b << 16) | (c << 8) | d) & m;
    *mask_host = m;
    return true;
}

static bool name_ok(const char *s) {
    for (; *s; s++) {
        char c = *s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' ||
              c == '_' || c == '.' || c == '(' || c == ')'))
            return false;
    }
    return true;
}

bool radio_check_links(const radio_link_t links[RADIO_MAX], uint32_t shown_mask, char *err, size_t err_len) {
    shown_mask |= 1u;  // radio 0 always listens
    for (int i = 0; i < RADIO_MAX; i++) {
        const radio_link_t *l = &links[i];
        bool shown = (shown_mask >> i) & 1u;
        if (memchr(l->name, 0, sizeof(l->name)) == NULL || !name_ok(l->name)) {
            snprintf(err, err_len, "Radio %d: name may use letters, digits, space and - _ . ( ) (up to 15)", i + 1);
            return false;
        }
        if (l->role > RADIO_ROLE_MAX) {
            snprintf(err, err_len, "Radio %d: role is zigbee, zha, thread or zwave", i + 1);
            return false;
        }
        // Radio 1 carries the dashboard's Zigbee2MQTT view (coordinator stats, device table, health)
        if (i == 0 && l->role == RADIO_ROLE_ZHA) {
            snprintf(err, err_len, "Radio 1 is the Zigbee2MQTT radio: use another radio for ZHA");
            return false;
        }
        if (!shown) continue;  // a radio nobody uses keeps what it has; it gets a free port when a stick takes it
        if (l->tcp_port < 1 || l->tcp_port == 80) {
            snprintf(err, err_len, "Radio %d: TCP port must be 1-65535 and not 80", i + 1);
            return false;
        }
        for (int j = 0; j < i; j++) {
            if (((shown_mask >> j) & 1u) && links[j].tcp_port == l->tcp_port) {
                snprintf(err, err_len, "Radio %d: TCP port %u is already used by radio %d", i + 1, l->tcp_port, j + 1);
                return false;
            }
        }
        if (l->baud < 9600 || l->baud > 921600) {
            snprintf(err, err_len, "Radio %d: baud rate must be 9600-921600", i + 1);
            return false;
        }
        uint32_t a, m;
        if (memchr(l->allow, 0, sizeof(l->allow)) == NULL || !radio_parse_allow(l->allow, &a, &m)) {
            snprintf(err, err_len, "Radio %d: allowed host: use a.b.c.d, a.b.c.d/nn or leave empty", i + 1);
            return false;
        }
    }
    return true;
}

bool radio_hub_reforce(bool fs_only, bool is_hub, bool high_speed, int64_t now_us, int64_t *last_us) {
    if (!fs_only || !is_hub || !high_speed) return false;
    if (*last_us != 0 && now_us - *last_us < RADIO_REFORCE_US) return false;
    *last_us = now_us ? now_us : 1;  // 0 means "never"
    return true;
}
