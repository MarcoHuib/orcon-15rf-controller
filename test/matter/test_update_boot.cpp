// Host test of the update restart rules (src/fw_update.h: updateBootRefusal, updateBootDone).
//   test/run_host_tests.sh (matter_update_boot)
#include "fw_update.h"
#include <cstdio>
#include <cstring>

using namespace fwup;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)
static bool is(const char* a, const char* b) { return a && b && !std::strcmp(a, b); }

int main() {
    // matterBuild, roomNow, jobBusy, restartPending, trialBoot, matterBusy, sessionBusy
    const UpdateBootAsk ok{true, false, false, false, false, false, false};
    CHECK(updateBootRefusal(ok) == nullptr);                       // Matter runs, no room: the restart is the way
    UpdateBootAsk a = ok; a.matterBuild = false; CHECK(is(updateBootRefusal(a), "not-needed"));
    a = ok; a.roomNow = true;        CHECK(is(updateBootRefusal(a), "not-needed"));      // the check works in place
    a = ok; a.jobBusy = true;        CHECK(is(updateBootRefusal(a), "update-busy"));
    a = ok; a.restartPending = true; CHECK(is(updateBootRefusal(a), "update-busy"));
    a = ok; a.trialBoot = true;      CHECK(is(updateBootRefusal(a), "confirm-running-image-first"));   // never in a trial
    a = ok; a.matterBusy = true;     CHECK(is(updateBootRefusal(a), "matter-busy"));
    a = ok; a.sessionBusy = true;    CHECK(is(updateBootRefusal(a), "session-busy"));
    a = ok; a.trialBoot = a.jobBusy = true; CHECK(is(updateBootRefusal(a), "update-busy"));   // first refusal wins

    // In the update boot: back to normal 60 s after a check without a newer release, at most after 10 minutes,
    // never while busy (a download, its restart, an RF command).
    CHECK(!updateBootDone(5000, false, false, 0, false));                 // not checked yet
    CHECK(!updateBootDone(70000, true, false, 30000, false));             // 40 s after the check: the page shows why
    CHECK(updateBootDone(90000, true, false, 30000, false));              // 60 s after it: back
    CHECK(!updateBootDone(90000, true, true, 30000, false));              // newer found: the owner may install
    CHECK(!updateBootDone(UPDATE_BOOT_MAX_MS - 1, true, true, 30000, false));
    CHECK(updateBootDone(UPDATE_BOOT_MAX_MS, true, true, 30000, false));  // at most 10 minutes without Matter
    CHECK(updateBootDone(UPDATE_BOOT_MAX_MS, false, false, 0, false));    // also when the check never ran (no network)
    CHECK(!updateBootDone(UPDATE_BOOT_MAX_MS + 1, true, false, 0, true)); // a download still runs: never cut it off
    CHECK(!updateBootDone(90000, true, false, 30000, true));

    std::printf(fails ? "update boot: %d FAILED\n" : "update boot: all checks passed\n", fails);
    return fails ? 1 : 0;
}
