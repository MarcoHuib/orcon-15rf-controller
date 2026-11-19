// reset_gesture.h — the physical recovery menu on the Heltec V2 buttons. Pure logic (host-tested:
// test/test_reset_gesture.cpp); net.cpp feeds it the PRG level and draws the OLED.
//
// Hardware: RST is the ESP32 EN pin (a hard reset, never readable by software). PRG is GPIO0, the
// boot strapping pin: held while RST is released, it selects the ROM download mode (the firmware
// does not run, the OLED stays dark; a plain RST press recovers). So the gesture is RST first,
// release it, and only then hold PRG:
//   1. press RST and let go (the board restarts);
//   2. within 20 s of the restart, hold PRG for 15 s (the OLED counts down after 1 s);
//   3. the menu opens; release PRG. Short press = next option, hold 2 s = choose;
//   4. "Fabrieksreset" needs a separate confirmation: release, then hold PRG another 5 s.
// The default option is "Annuleren"; a minute without input closes the menu; a stuck button can
// never choose anything (every step needs a new press after a release). A short PRG press
// outside this sequence only wakes the OLED, exactly as before.
#pragma once
#include <stdint.h>

namespace gesture {
constexpr uint32_t DEBOUNCE_MS = 30;
constexpr uint32_t WINDOW_MS = 20000;      // the hold must start within 20 s after boot
constexpr uint32_t HOLD_MS = 15000;        // and last 15 s
constexpr uint32_t SHOW_AFTER_MS = 1000;   // a short wake press never flashes the countdown
constexpr uint32_t CHOOSE_MS = 2000;       // menu: hold 2 s = choose; shorter = next option
constexpr uint32_t CONFIRM_MS = 5000;      // factory reset: hold 5 s more to confirm
constexpr uint32_t IDLE_MS = 60000;        // no input for a minute: the menu closes, nothing happens

// Menu order: the safe choice first, the destructive one last.
enum class Option : uint8_t { Cancel, Recovery, WifiReset, FactoryReset, Count };
enum class Action : uint8_t { None, Wake, Cancelled, Recovery, WifiReset, FactoryReset };
enum class Phase : uint8_t { Idle, Arming, Release, Menu, Confirm };

class Gesture {
public:
    // Call on every loop pass with the raw PRG level (true = released: INPUT_PULLUP, active low).
    Action poll(bool released, uint32_t now) {
        if (now >= WINDOW_MS) window_ = false;          // latched: a millis() wrap never reopens it
        bool press = false, release = false;
        if (released != raw_) { raw_ = released; changed_ = now; }
        else if (released != stable_ && uint32_t(now - changed_) >= DEBOUNCE_MS) {
            stable_ = released;
            (stable_ ? release : press) = true;
        }
        const bool held = !stable_;
        switch (phase_) {
            case Phase::Idle:
                if (!press) return Action::None;
                if (window_) { phase_ = Phase::Arming; since_ = now; }
                return Action::Wake;                    // the OLED wake of a normal press
            case Phase::Arming:
                if (release) { const bool shown = visible(now); phase_ = Phase::Idle; return shown ? Action::Cancelled : Action::None; }
                if (uint32_t(now - since_) >= HOLD_MS) { phase_ = Phase::Release; opt_ = Option::Cancel; input_ = now; }
                return Action::None;
            case Phase::Release:                        // the menu is open; wait for the hold to end
                if (release) { phase_ = Phase::Menu; input_ = now; pressAt_ = 0; down_ = false; }
                return idle(now);
            case Phase::Menu:
                if (press) { down_ = true; pressAt_ = now; input_ = now; }
                if (down_ && held && uint32_t(now - pressAt_) >= CHOOSE_MS) return choose(now);
                if (down_ && release) { down_ = false; input_ = now; opt_ = Option((uint8_t(opt_) + 1) % uint8_t(Option::Count)); }
                return down_ ? Action::None : idle(now);
            case Phase::Confirm:                        // first the choosing hold must end, then a new 5 s hold
                if (!armed_) { if (release) { armed_ = true; input_ = now; } return idle(now); }
                if (press) { down_ = true; pressAt_ = now; input_ = now; }
                if (down_ && held && uint32_t(now - pressAt_) >= CONFIRM_MS) { phase_ = Phase::Idle; down_ = false; return Action::FactoryReset; }
                if (down_ && release) { phase_ = Phase::Idle; down_ = false; return Action::Cancelled; }
                return down_ ? Action::None : idle(now);
        }
        return Action::None;
    }
    Phase phase() const { return phase_; }
    Option option() const { return opt_; }
    bool held() const { return !stable_; }
    bool confirming() const { return phase_ == Phase::Confirm && armed_ && down_; }
    // The OLED shows the gesture (and stays awake) while this is true.
    bool visible(uint32_t now) const {
        return phase_ == Phase::Arming ? uint32_t(now - since_) >= SHOW_AFTER_MS : phase_ != Phase::Idle;
    }
    // Countdown while holding: to the menu (Arming) or to the factory reset (Confirm).
    uint32_t leftMs(uint32_t now) const {
        const uint32_t total = phase_ == Phase::Arming ? HOLD_MS : CONFIRM_MS, start = phase_ == Phase::Arming ? since_ : pressAt_;
        const uint32_t done = uint32_t(now - start);
        return done >= total ? 0 : total - done;
    }

private:
    Action idle(uint32_t now) {
        if (uint32_t(now - input_) < IDLE_MS) return Action::None;
        phase_ = Phase::Idle; down_ = false;
        return Action::Cancelled;
    }
    Action choose(uint32_t now) {
        down_ = false;
        switch (opt_) {
            case Option::FactoryReset: phase_ = Phase::Confirm; armed_ = false; input_ = now; return Action::None;
            case Option::Recovery: phase_ = Phase::Idle; return Action::Recovery;
            case Option::WifiReset: phase_ = Phase::Idle; return Action::WifiReset;
            default: phase_ = Phase::Idle; return Action::Cancelled;
        }
    }
    Phase phase_ = Phase::Idle;
    Option opt_ = Option::Cancel;
    bool raw_ = true, stable_ = true, window_ = true, down_ = false, armed_ = false;
    uint32_t changed_ = 0, since_ = 0, input_ = 0, pressAt_ = 0;
};
}  // namespace gesture
