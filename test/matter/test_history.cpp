#include "orcon_matter_history.h"

#include <assert.h>

using orcon_matter::MatterHistory;
using Kind = MatterHistory::Kind;

int main() {
    MatterHistory history;
    assert(!history.size() && !history.dropped && !history.tracking);
    for (unsigned index = 1; index < 255; ++index) assert(!history.added(index));
    history.setAdded(7, true);
    history.setAdded(8, true);
    history.setAdded(254, true);

    for (unsigned i = 0; i < 100; ++i) history.add(Kind::Complete, i, 7, 4937);
    assert(history.size() == MatterHistory::capacity);
    assert(history.dropped == 100 - MatterHistory::capacity);
    for (unsigned i = 0; i < history.size(); ++i) {
        const auto& event = history.at(i);
        assert(event.kind == Kind::Complete && event.ms == history.dropped + i);
        assert(event.index == 7 && event.vendor == 4937 && event.error == 0);
    }

    history.add(Kind::LocalRemoveError, 100, 254, 0, 0x1234);
    history.setAdded(7, false);
    assert(!history.added(7) && history.added(8) && history.added(254));
    const auto& last = history.at(history.size() - 1);
    assert(last.kind == Kind::LocalRemoveError && last.error == 0x1234);
}
