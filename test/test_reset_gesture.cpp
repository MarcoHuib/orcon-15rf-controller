// The physical recovery menu (src/reset_gesture.h): every button sequence of the procedure, plus
// the ways it must NOT trigger (normal wake press, late press, stuck button, bounce, millis wrap).
#include "reset_gesture.h"
#include <cstdio>
#include <vector>
using namespace gesture;
static int fails = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

// Drives a Gesture with a 10 ms loop and records every non-None action.
struct Rig {
    Gesture g;
    uint32_t t = 0;
    std::vector<Action> acts;
    void run(bool released, uint32_t ms) {
        for (uint32_t i = 0; i < ms; i += 10, t += 10) { const Action a = g.poll(released, t); if (a != Action::None) acts.push_back(a); }   // t may wrap
    }
    void hold(uint32_t ms) { run(false, ms); }
    void idle(uint32_t ms) { run(true, ms); }
    void tap() { hold(200); idle(300); }
    int count(Action a) const { int n = 0; for (Action x : acts) n += x == a; return n; }
    bool only(std::initializer_list<Action> want) const { return acts == std::vector<Action>(want); }
};
// Opens the menu: boot, 2 s later hold PRG 15 s (+ debounce), release.
static void openMenu(Rig& r) { r.idle(2000); r.hold(15100); r.idle(300); }

int main() {
    {   // A normal press, any time: only the OLED wake, never a menu.
        Rig r; r.idle(30000); r.tap(); r.hold(20000); r.idle(500);
        CHECK(r.only({Action::Wake, Action::Wake}) && r.g.phase() == Phase::Idle);
    }
    {   // Short reset only (no PRG): nothing at all.
        Rig r; r.idle(40000); CHECK(r.acts.empty() && r.g.phase() == Phase::Idle);
    }
    {   // Short press inside the boot window: a wake, no countdown shown, nothing else.
        Rig r; r.idle(3000); r.hold(400); CHECK(!r.g.visible(r.t)); r.idle(500);
        CHECK(r.only({Action::Wake}) && r.g.phase() == Phase::Idle);
    }
    {   // Long press released before 15 s: countdown shown after 1 s, then cancelled.
        Rig r; r.idle(2000); r.hold(1100); CHECK(r.g.visible(r.t) && r.g.phase() == Phase::Arming);
        CHECK(r.g.leftMs(r.t) > 13000 && r.g.leftMs(r.t) <= 14000);
        r.hold(6000); r.idle(500);
        CHECK(r.only({Action::Wake, Action::Cancelled}) && r.g.phase() == Phase::Idle && !r.g.visible(r.t));
    }
    {   // 15 s hold opens the menu, on "Annuleren"; it stays put until PRG is released.
        Rig r; r.idle(2000); r.hold(15100);
        CHECK(r.g.phase() == Phase::Release && r.g.option() == Option::Cancel && r.g.visible(r.t));
        r.idle(300); CHECK(r.g.phase() == Phase::Menu);
        r.hold(2100); r.idle(200);                      // choosing the default = cancel
        CHECK(r.only({Action::Wake, Action::Cancelled}) && r.g.phase() == Phase::Idle);
    }
    {   // Herstelmodus: one short press, then hold 2 s.
        Rig r; openMenu(r); r.tap(); CHECK(r.g.option() == Option::Recovery);
        r.hold(1900); CHECK(r.count(Action::Recovery) == 0);   // not yet
        r.hold(200); CHECK(r.count(Action::Recovery) == 1 && r.g.phase() == Phase::Idle);
    }
    {   // WiFi wissen: two short presses, then hold.
        Rig r; openMenu(r); r.tap(); r.tap(); CHECK(r.g.option() == Option::WifiReset);
        r.hold(2100); CHECK(r.only({Action::Wake, Action::WifiReset}));
    }
    {   // Fabrieksreset: choosing it only asks for confirmation; a separate 5 s hold confirms.
        Rig r; openMenu(r); r.tap(); r.tap(); r.tap(); CHECK(r.g.option() == Option::FactoryReset);
        r.hold(2100); CHECK(r.g.phase() == Phase::Confirm && r.count(Action::FactoryReset) == 0);
        r.hold(6000); CHECK(r.count(Action::FactoryReset) == 0);   // still the choosing hold: never counts
        r.idle(300); r.hold(1000); CHECK(r.g.confirming() && r.g.leftMs(r.t) > 3900 && r.g.leftMs(r.t) <= 4050);   // + debounce
        r.hold(4100); CHECK(r.count(Action::FactoryReset) == 1 && r.g.phase() == Phase::Idle);
    }
    {   // Fabrieksreset cancelled: a short press in the confirmation.
        Rig r; openMenu(r); r.tap(); r.tap(); r.tap(); r.hold(2100); r.idle(300); r.tap();
        CHECK(r.count(Action::FactoryReset) == 0 && r.acts.back() == Action::Cancelled && r.g.phase() == Phase::Idle);
    }
    {   // Fabrieksreset confirmation times out without input.
        Rig r; openMenu(r); r.tap(); r.tap(); r.tap(); r.hold(2100); r.idle(61000);
        CHECK(r.count(Action::FactoryReset) == 0 && r.acts.back() == Action::Cancelled);
    }
    {   // The options wrap around; the menu closes itself after a minute without input.
        Rig r; openMenu(r); for (int i = 0; i < 4; ++i) r.tap();
        CHECK(r.g.option() == Option::Cancel); r.idle(61000);
        CHECK(r.acts.back() == Action::Cancelled && r.g.phase() == Phase::Idle);
    }
    {   // A stuck button: the menu opens but nothing is ever chosen; it closes after a minute.
        Rig r; r.idle(2000); r.hold(200000);
        CHECK(r.count(Action::Recovery) + r.count(Action::WifiReset) + r.count(Action::FactoryReset) == 0);
        CHECK(r.g.phase() == Phase::Idle && r.acts.back() == Action::Cancelled);
    }
    {   // Pressed before boot finished polling (already low at the first sample): counts from then.
        Rig r; r.hold(15100); CHECK(r.g.phase() == Phase::Release);
    }
    {   // A hold that starts after the 20 s window never opens the menu.
        Rig r; r.idle(20500); r.hold(30000); r.idle(300);
        CHECK(r.only({Action::Wake}) && r.g.phase() == Phase::Idle);
    }
    {   // millis() wraps after 49.7 days: the boot window does not reopen.
        Rig r; r.idle(25000); r.t = 0xFFFFFF00u; r.idle(1000);   // now just past the wrap (t small)
        CHECK(r.t < 1000); r.hold(16000); r.idle(300);
        CHECK(r.g.phase() == Phase::Idle && r.count(Action::Wake) == 1 && r.count(Action::Cancelled) == 0);
    }
    {   // Contact bounce (5 ms flicker) never registers as presses; one clean press does.
        Rig r; r.idle(25000);
        for (int i = 0; i < 40; ++i) { r.g.poll(i & 1, r.t); r.t += 5; }
        r.idle(100); CHECK(r.acts.empty());
        r.tap(); CHECK(r.only({Action::Wake}));
    }
    printf("reset gesture: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
