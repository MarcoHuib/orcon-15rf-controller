// rx_stream.cpp — see rx_stream.h
#include "rx_stream.h"
#include <string.h>

namespace rxs {

bool Decoder::put(uint8_t b) {
    if (len_ == sizeof(buf_)) { ++resets; base_ += len_; len_ = 0; }   // caller ignored room()
    buf_[len_++] = b;
    ++bytes;
    bool done = false;
    // Track the core sync 00 33 55 53 and the byte before it (FF only if the UART
    // happened to be byte-aligned on the preamble).
    if (b == ramses::SYNC[1 + progress_]) {
        if (progress_ == 0) pre_ = prev_;
        if (++progress_ == 4) {
            progress_ = 0;
            Sync& s = ring_[syncs % RING];
            s.at = base_ + len_ - 4;            // stream offset of the 00 byte
            s.pre = pre_; s.ms = 0; s.rssi = 0;
            ++syncs;
            if (pre_ == ramses::SYNC[0]) ++syncsFF;
            done = true;
        }
    } else if (b == ramses::SYNC[1]) {
        progress_ = 1; pre_ = prev_;
    } else {
        progress_ = 0;
    }
    prev_ = b;
    return done;
}

void Decoder::noteSync(int16_t rssi, uint32_t ms) {
    Sync& s = ring_[(syncs - 1) % RING];
    s.rssi = rssi; s.ms = ms;
}

bool Decoder::next(Item& out) {
    if (len_ < 8) return false;
    size_t consumed = 0, at = 0;
    bool ok = ramses::extractWireFrame(buf_, len_, &consumed, out.f, &at);
    if (ok) {
        ++frames;
        out.seq = 0; out.ms = 0; out.rssi = 0;
        for (uint32_t k = 0; k < RING && k < syncs; k++) {          // tag with its own sync
            const Sync& s = ring_[(syncs - 1 - k) % RING];
            if (s.at == base_ + at) { out.seq = syncs - k; out.ms = s.ms; out.rssi = s.rssi; break; }
        }
    }
    if (consumed > 0 && consumed <= len_) {
        memmove(buf_, buf_ + consumed, len_ - consumed);
        len_ -= consumed; base_ += consumed;
    } else if (len_ >= sizeof(buf_)) {
        ++resets; base_ += len_; len_ = 0;                           // overflow guard
    }
    return ok;
}

} // namespace rxs
