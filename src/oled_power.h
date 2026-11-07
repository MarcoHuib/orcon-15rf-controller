// OLED-only power policy. No ESP32 sleep, radio or network dependencies.
#pragma once
#include <stdint.h>
#include <string.h>

namespace oledpower {
enum class Mode { On, Off, Auto };
constexpr uint32_t TIMEOUT_MS = 30000;

inline const char* modeName(Mode mode) {
    switch (mode) {
        case Mode::On: return "on";
        case Mode::Off: return "off";
        default: return "auto";
    }
}

inline bool parseMode(const char* value, Mode& mode) {
    if (!value) return false;
    if (!strcmp(value, "on")) mode = Mode::On;
    else if (!strcmp(value, "off")) mode = Mode::Off;
    else if (!strcmp(value, "auto")) mode = Mode::Auto;
    else return false;
    return true;
}

// Missing/invalid NVS settings use the energy-saving default on Heltec V2.
inline Mode storedMode(const char* value) {
    Mode mode = Mode::Auto;
    parseMode(value, mode);
    return mode;
}

class Power {
public:
    // Explicitly applying Auto starts a fresh 30-second viewing window, even
    // when already in Auto. Draws, RF traffic and HTTP reads never restart it.
    void setMode(Mode mode, uint32_t now) {
        mode_ = mode;
        since_ = now;
        awake_ = mode != Mode::Off;
    }
    void tick(uint32_t now) {
        // Only On is timeless. Auto runs its window from setMode/wake; a manual
        // wake in Off gets that same one-shot 30 s window (then latches off).
        if (mode_ != Mode::On && awake_ && uint32_t(now - since_) >= TIMEOUT_MS)
            awake_ = false;  // latch off: a later millis() wrap must not wake it
    }
    // Short PRG press / web toggle "on": restart the viewing window. Never
    // persists and never changes the mode; background traffic must not call this.
    void wake(uint32_t now) { awake_ = true; since_ = now; }
    // Web toggle "off": asleep in every mode until wake(), setMode() or reboot.
    void sleep() { awake_ = false; }
    // Call after U8g2 begin() (which starts awake). Only send a display command
    // on a transition; the template also lets host tests observe driver calls.
    template <typename Oled>
    bool sync(Oled& oled, uint32_t now) {
        tick(now);
        if (appliedAwake_ == awake_) return false;
        oled.setPowerSave(awake_ ? 0 : 1);
        appliedAwake_ = awake_;
        return true;
    }
    Mode mode() const { return mode_; }
    bool awake() const { return awake_; }

private:
    Mode mode_ = Mode::Auto;
    uint32_t since_ = 0;
    bool awake_ = true;
    bool appliedAwake_ = true;
};
} // namespace oledpower
