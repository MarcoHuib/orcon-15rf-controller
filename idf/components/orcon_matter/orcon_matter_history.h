#pragma once

#include <stdint.h>

namespace orcon_matter {

// CHIP stack lock protects this history. Only local indices, public vendor IDs and relative times are retained.
class MatterHistory {
public:
    enum class Kind : uint8_t {
        Ready, WindowOpen, WindowClosed, PaseStarted, PaseStopped, Complete, FailSafeExpired,
        FabricPending, FabricCommitted, FabricRemoved, LocalRemoveError, DnssdReady,
    };
    struct Event {
        uint32_t ms;
        uint32_t error;
        uint16_t vendor;
        uint8_t index;
        Kind kind;
    };
    static constexpr unsigned capacity = 24;

    void add(Kind kind, uint32_t ms, uint8_t index = 0, uint16_t vendor = 0, uint32_t error = 0) {
        events_[next_] = {ms, error, vendor, index, kind};
        next_ = (next_ + 1) % capacity;
        if (size_ < capacity) ++size_;
        else ++dropped;
    }
    unsigned size() const { return size_; }
    const Event& at(unsigned i) const { return events_[(next_ + capacity - size_ + i) % capacity]; }
    bool added(unsigned index) const { return added_[index / 8] & (1u << (index % 8)); }
    void setAdded(unsigned index, bool present) {
        const unsigned bit = 1u << (index % 8);
        if (present) added_[index / 8] |= bit;
        else added_[index / 8] &= ~bit;
    }

    uint32_t dropped = 0;
    uint32_t attempts = 0;
    uint32_t revision = 0;
    bool tracking = false;
    bool pase = false;

private:
    Event events_[capacity]{};
    uint8_t added_[32]{};
    unsigned next_ = 0;
    unsigned size_ = 0;
};

}  // namespace orcon_matter
