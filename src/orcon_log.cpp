// orcon_log.cpp — see orcon_log.h
#include "orcon_log.h"
#include <Arduino.h>
#include <stdio.h>
#include <string.h>

namespace rlog {

struct Entry {
    uint32_t t;
    char     dir;
    int16_t  rssi;
    char     str[96];
};

static Entry  g_buf[CAP];
static size_t g_head = 0;
static size_t g_count = 0;
static Seen   g_seen;
static Device g_dev[MAX_DEVICES];
static size_t g_devN = 0;

static const char* msgType(ramses::MsgType t) {
    switch (t) {
        case ramses::MsgType::I:  return "I ";
        case ramses::MsgType::RQ: return "RQ";
        case ramses::MsgType::RP: return "RP";
        case ramses::MsgType::W:  return "W ";
    }
    return "??";
}

static void addrStr(const ramses::Frame& f, int i, char* out) {
    if (f.has_addr[i])
        snprintf(out, 12, "%02u:%06lu", f.addr[i].cls, (unsigned long)f.addr[i].serial);
    else
        strcpy(out, "--:------");
}

size_t format(const ramses::Frame& f, char* out, size_t cap) {
    char a0[12], a1[12], a2[12];
    addrStr(f, 0, a0); addrStr(f, 1, a1); addrStr(f, 2, a2);
    int n = snprintf(out, cap, "%s --- %s %s %s %04X %03u ",
                     msgType(f.type), a0, a1, a2, f.cmd, f.len);
    for (uint8_t i = 0; i < f.len && (size_t)n < cap - 3; i++)
        n += snprintf(out + n, cap - n, "%02X", f.payload[i]);
    return (size_t)n;
}

static void noteDevice(const ramses::DeviceId& id, uint8_t role, uint16_t cmd, int rssi);

static void learn(const ramses::Frame& f, int rssi) {
    // Roles come from what a device SENDS, not its class: on the MVS-15RHBP the
    // fan AND its remote are both class 29 (HRC400 fans are 32), e.g.
    //   I 29:000101 -> 29:000202 22F1 000104  (remote -> fan)
    //   I 29:000202 ...          31D9 000003  (fan state broadcast)
    if (!f.has_addr[0]) return;
    const ramses::DeviceId& src = f.addr[0];
    uint8_t role = 0;
    if ((f.cmd == 0x31D9 || f.cmd == 0x31DA) &&            // only a ventilation unit reports these
        f.type != ramses::MsgType::RQ) {                   // (an RQ for them comes from a controller)
        g_seen.fan = src; g_seen.haveFan = true;
        role = ROLE_UNIT;
    } else if ((f.cmd == 0x22F1 || f.cmd == 0x22F3) &&     // fan-mode / timer command
               f.type != ramses::MsgType::RP) {
        g_seen.remote = src; g_seen.haveRemote = true;
        role = ROLE_REMOTE;
        int dst = f.has_addr[1] ? 1 : (f.has_addr[2] ? 2 : -1);
        if (dst > 0 && !(f.addr[dst] == src)) { g_seen.fan = f.addr[dst]; g_seen.haveFan = true; }
    }
    noteDevice(src, role, f.cmd, rssi);
}

static void noteDevice(const ramses::DeviceId& id, uint8_t role, uint16_t cmd, int rssi) {
    size_t k = 0;
    while (k < g_devN && !(g_dev[k].id == id)) k++;
    const uint32_t now = millis();
    if (k == g_devN) {
        if (g_devN < MAX_DEVICES) {
            g_devN++;
        } else {                                   // full: reuse the least recently heard slot
            k = 0;
            for (size_t i = 1; i < MAX_DEVICES; i++)
                if (now - g_dev[i].lastMs > now - g_dev[k].lastMs) k = i;
        }
        g_dev[k] = Device(); g_dev[k].id = id;
    }
    Device& d = g_dev[k];
    d.roles |= role; d.lastCmd = cmd; d.count++; d.lastMs = now; d.rssi = (int16_t)rssi;
}

size_t devices(const Device** out) { if (out) *out = g_dev; return g_devN; }

size_t devicesJson(char* out, size_t cap) {
    static const char* const ROLE[] = { "other", "unit", "remote", "unit+remote" };
    size_t n = 0;
    if (cap < 3) return 0;
    out[n++] = '[';
    for (size_t k = 0; k < g_devN && n + 100 < cap; k++) {
        const Device& d = g_dev[k];
        n += snprintf(out + n, cap - n, "%s{\"id\":\"%02u:%06lu\",\"role\":\"%s\",\"n\":%lu,\"age_s\":%lu,\"rssi\":%d,\"cmd\":\"%04X\"}",
                      k ? "," : "", d.id.cls, (unsigned long)d.id.serial, ROLE[d.roles & 3], (unsigned long)d.count,
                      (unsigned long)((millis() - d.lastMs) / 1000), d.rssi, d.lastCmd);
    }
    out[n++] = ']'; out[n] = 0;
    return n;
}

void add(char dir, const ramses::Frame& f, int rssi) {
    Entry& e = g_buf[g_head];
    e.t = millis();
    e.dir = dir;
    e.rssi = (int16_t)rssi;
    format(f, e.str, sizeof(e.str));
    g_head = (g_head + 1) % CAP;
    if (g_count < CAP) g_count++;
    if (dir == 'R') learn(f, rssi);
}

size_t toJson(char* out, size_t cap) {
    size_t n = 0;
    out[n++] = '[';
    size_t start = (g_head + CAP - g_count) % CAP;
    for (size_t k = 0; k < g_count; k++) {
        size_t idx = (start + k) % CAP;
        Entry& e = g_buf[idx];
        if (n + 160 >= cap) break;           // one entry (<= ~140 B) + "]" + NUL must fit
        if (k) out[n++] = ',';
        n += snprintf(out + n, cap - n,
                      "{\"t\":%lu,\"d\":\"%c\",\"r\":%d,\"s\":\"%s\"}",
                      (unsigned long)e.t, e.dir, e.rssi, e.str);
    }
    out[n++] = ']';
    out[n] = '\0';
    return n;
}

const Seen& seen() { return g_seen; }

} // namespace rlog
