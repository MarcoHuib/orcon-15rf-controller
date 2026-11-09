#include "oled_power.h"
#include <cstdio>
#include <initializer_list>

using namespace oledpower;
static int fails = 0;
#define CHECK(c,m) do { if (!(c)) { printf("  [FAIL] %s\n",m); ++fails; } } while (0)

struct FakeOled {
    int calls = 0;
    int powerSave = 0;
    void setPowerSave(uint8_t value) { ++calls; powerSave = value; }
};

int main() {
    CHECK(storedMode(nullptr) == Mode::Auto && storedMode("") == Mode::Auto &&
          storedMode("corrupt") == Mode::Auto, "missing/invalid stored mode defaults to Auto");
    for (Mode mode : {Mode::On, Mode::Off, Mode::Auto}) {
        CHECK(storedMode(modeName(mode)) == mode, "all saved modes round-trip across boot");
    }
    Mode mode = Mode::Off;
    for (const char* bad : {"", "AUTO", "On", "true", "0", "auto ", "sleep"}) {
        CHECK(!parseMode(bad, mode) && mode == Mode::Off, "invalid values leave mode untouched");
    }

    Power p;
    p.setMode(storedMode(nullptr), 1000);
    p.tick(30999);
    CHECK(p.awake(), "Auto stays on until 29999 ms");
    p.tick(31000);
    CHECK(!p.awake() && p.mode() == Mode::Auto, "Auto off at exactly 30000 ms");
    // Frequent service calls (e.g. status draws, RF frames and HTTP polling)
    // must not extend the viewing window or wake an expired screen.
    p.setMode(Mode::Auto, 200000);
    for (uint32_t now = 200000; now <= 260000; now += 10) {
        p.tick(now);
        CHECK(p.awake() == (now < 230000), "background activity cannot restart Auto");
    }
    p.tick(200001);  // a full clock cycle later
    CHECK(!p.awake(), "expired Auto remains off after another millis rollover");

    p.setMode(Mode::On, 500);
    for (uint32_t now : {500U, 30500U, 3600500U, UINT32_MAX, 0U}) {
        p.tick(now);
        CHECK(p.awake(), "On never times out, including at rollover");
    }
    p.setMode(Mode::Off, 50);
    for (uint32_t now : {50U, 30050U, UINT32_MAX, 0U}) {
        p.tick(now);
        CHECK(!p.awake(), "Off is immediate and remains off");
    }
    p.setMode(Mode::Auto, 70000);
    CHECK(p.awake(), "Off -> Auto wakes");
    p.tick(99999);
    p.setMode(Mode::Auto, 99999);
    p.tick(129998);
    CHECK(p.awake(), "explicit Apply Auto restarts viewing window");
    p.tick(129999);
    CHECK(!p.awake(), "restarted window expires");
    p.setMode(Mode::On, 130000);
    CHECK(p.awake(), "expired Auto -> On wakes immediately");
    p.setMode(Mode::Off, 130001);
    CHECK(!p.awake(), "On -> Off is immediate");

    const uint32_t start = UINT32_MAX - 10000;
    p.setMode(Mode::Auto, start);
    p.tick(uint32_t(start + 29999));
    CHECK(p.awake(), "Auto interval spans millis rollover");
    p.tick(uint32_t(start + 30000));
    CHECK(!p.awake(), "Auto expires at 30 seconds across rollover");
    p.setMode(storedMode("off"), 42);
    CHECK(!p.awake(), "persisted Off is applied at startup");
    p.setMode(storedMode("on"), 42);
    p.tick(100000);
    CHECK(p.awake(), "persisted On survives startup timeout");

    Power driver;
    FakeOled oled;
    driver.setMode(Mode::Auto, 1000);
    CHECK(!driver.sync(oled, 30999) && oled.calls == 0, "no driver command before deadline");
    CHECK(driver.sync(oled, 31000) && oled.calls == 1 && oled.powerSave == 1,
          "deadline sends U8g2 power-save enable");
    for (uint32_t now = 31001; now < 40000; ++now) driver.sync(oled, now);
    CHECK(oled.calls == 1, "sleeping screen receives no repeated power commands");
    driver.setMode(Mode::On, 40000);
    CHECK(driver.sync(oled, 40000) && oled.calls == 2 && oled.powerSave == 0,
          "On disables U8g2 power-save immediately");
    driver.setMode(Mode::Off, 40001);
    CHECK(driver.sync(oled, 40001) && oled.calls == 3 && oled.powerSave == 1,
          "Off enables U8g2 power-save immediately");
    driver.setMode(Mode::Auto, 40002);
    CHECK(driver.sync(oled, 40002) && oled.calls == 4 && oled.powerSave == 0,
          "Auto wakes a sleeping screen");
    CHECK(driver.sync(oled, 70002) && oled.calls == 5 && oled.powerSave == 1,
          "woken Auto returns to power-save after 30 seconds");

    Power bootOff;
    FakeOled bootDisplay;
    bootOff.setMode(storedMode("off"), 42);
    CHECK(bootOff.sync(bootDisplay, 42) && bootDisplay.powerSave == 1,
          "stored Off puts the initially awake U8g2 display to sleep at startup");
    CHECK(!bootOff.sync(bootDisplay, 30042) && bootDisplay.calls == 1,
          "stored Off stays asleep without repeated driver commands");

    // Manual wake/sleep (PRG button / web toggle): transient actions, never a
    // mode change, never persisted.
    Power manual;
    manual.setMode(Mode::Auto, 1000);
    manual.tick(31000);
    CHECK(!manual.awake(), "Auto expired before manual wake");
    manual.wake(50000);
    CHECK(manual.awake() && manual.mode() == Mode::Auto,
          "PRG-style wake revives expired Auto without changing the mode");
    manual.tick(79999);
    CHECK(manual.awake(), "manual wake starts a fresh 30 s window");
    manual.tick(80000);
    CHECK(!manual.awake(), "manual window expires like an applied one");
    manual.sleep();
    for (uint32_t now = 80000; now <= 120000; now += 7) manual.tick(now);
    CHECK(!manual.awake(), "manual sleep latches off under background ticks");
    manual.wake(120000);
    CHECK(manual.awake(), "wake after manual sleep works");

    // Off: a manual wake is a 30 s peek, not a mode change; the stored policy
    // (dark at boot) is untouched.
    Power offPeek;
    offPeek.setMode(Mode::Off, 42);
    CHECK(!offPeek.awake(), "Off boots dark");
    offPeek.wake(1000);
    CHECK(offPeek.awake() && offPeek.mode() == Mode::Off,
          "PRG wake in Off lights the screen temporarily");
    offPeek.tick(30999);
    CHECK(offPeek.awake(), "Off peek lasts up to the 30 s window");
    offPeek.tick(31000);
    CHECK(!offPeek.awake(), "Off peek returns to dark and latches");
    offPeek.tick(200000);
    CHECK(!offPeek.awake(), "no auto re-wake after an Off peek");
    CHECK(offPeek.mode() == Mode::Off, "wake never changes the Off mode");

    // On: manual sleep holds dark until wake/apply/reboot; On stays timeless.
    Power onHold;
    onHold.setMode(Mode::On, 0);
    onHold.sleep();
    for (uint32_t now : {0U, 30000U, 300000U, UINT32_MAX}) {
        onHold.tick(now);
        CHECK(!onHold.awake(), "manual sleep in On holds dark");
    }
    onHold.wake(400000);
    onHold.tick(1000000);
    CHECK(onHold.awake(), "On after a manual wake stays on past 30 s");
    CHECK(onHold.mode() == Mode::On, "sleep/wake never changes the On mode");

    // Driver commands for the manual actions: exactly one command per transition.
    Power drv;
    FakeOled o2;
    drv.setMode(Mode::On, 1000);
    drv.wake(1000);
    CHECK(!drv.sync(o2, 1000) && o2.calls == 0, "wake while already awake sends no driver command");
    drv.sleep();
    CHECK(drv.sync(o2, 1001) && o2.calls == 1 && o2.powerSave == 1,
          "manual sleep sends one power-save enable");
    drv.sleep();
    CHECK(!drv.sync(o2, 1002) && o2.calls == 1, "repeated sleep is a no-op");
    drv.wake(1003);
    CHECK(drv.sync(o2, 1003) && o2.calls == 2 && o2.powerSave == 0,
          "manual wake sends one power-save disable");
    CHECK(!drv.sync(o2, 5000) && o2.calls == 2, "no repeated wake commands within the window");

    // Off-mode peek spanning a millis() rollover.
    Power roll;
    FakeOled o3;
    roll.setMode(Mode::Off, 10);
    CHECK(roll.sync(o3, 10) && o3.powerSave == 1, "stored Off applied at start");
    const uint32_t rw = UINT32_MAX - 10000;
    roll.wake(rw);
    CHECK(roll.sync(o3, rw) && o3.powerSave == 0, "Off wake across rollover wakes the driver");
    roll.tick(uint32_t(rw + 29999));
    CHECK(roll.awake(), "Off peek survives up to the rollover boundary");
    roll.tick(uint32_t(rw + 30000));
    CHECK(!roll.awake(), "Off peek expires across the millis rollover");

    printf("%s (%d failures)\n", fails ? "FAILED" : "ALL PASSED", fails);
    return fails ? 1 : 0;
}
