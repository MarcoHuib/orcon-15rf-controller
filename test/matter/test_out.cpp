// Host test of idf/components/orcon_matter/orcon_matter_out.h: the bounded text that every Matter answer and [mem]
// line is built in (CodeQL cpp/overflowing-snprintf, alert 4). Each case runs in a buffer with guard bytes behind it:
// nothing may be written at or past cap, the text stays NUL-terminated and holds only whole pieces.
//   test/run_host_tests.sh (matter_out)
#include "orcon_matter_out.h"

#include <cassert>
#include <climits>
#include <cstdio>
#include <cstring>
#include <string>

using orcon_matter::Out;

namespace {
constexpr size_t kGuard = 16;
struct Buf {   // cap bytes for Out, then guard bytes that must stay untouched
    char mem[256 + kGuard];
    size_t cap;
    explicit Buf(size_t c) : cap(c) {
        assert(c <= 256);
        memset(mem, 'Z', sizeof mem);
    }
    bool guardIntact() const {
        for (size_t i = cap; i < cap + kGuard; ++i)
            if (mem[i] != 'Z') return false;
        return true;
    }
};
void wholeText(const Buf& b, const Out& o, const char* want) {
    assert(b.guardIntact());
    assert(o.n < b.cap);
    assert(strlen(b.mem) == o.n);   // NUL-terminated at n
    assert(std::string(b.mem) == want);
}
}  // namespace

int main() {
    {   // a new Out starts as empty text
        Buf b(8);
        Out o{b.mem, b.cap};
        wholeText(b, o, "");
        assert(!o.full);
    }
    {   // exactly cap - 1 characters fit, with the NUL in the last byte
        Buf b(8);
        Out o{b.mem, b.cap};
        o.fmt("%s", "1234567");
        assert(!o.full);
        wholeText(b, o, "1234567");
    }
    {   // one more does not: the cut piece is dropped, never half kept
        Buf b(8);
        Out o{b.mem, b.cap};
        o.fmt("%s", "12345678");
        assert(o.full);
        wholeText(b, o, "");
    }
    {   // a piece that overflows after others keeps the earlier ones; later, smaller pieces are ignored
        Buf b(8);
        Out o{b.mem, b.cap};
        o.fmt("abc");
        o.fmt("%d", 12345);   // "abc" + 5 = 8 characters: one too many
        assert(o.full);
        wholeText(b, o, "abc");
        o.fmt("x");
        assert(o.full);
        wholeText(b, o, "abc");
    }
    {   // vsnprintf's negative answer (an encoding error) and an answer beyond the room: both end the text
        for (int k : {-1, INT_MIN, 5, 6, INT_MAX}) {
            Buf b(8);
            Out o{b.mem, b.cap};
            o.fmt("ab");   // 6 bytes left: an answer of 5 still fits, 6 or more does not
            o.settle(k);
            if (k == 5) {
                assert(!o.full && o.n == 7);
                continue;   // settle() alone: the buffer holds no 5 new characters here
            }
            assert(o.full);
            wholeText(b, o, "ab");
        }
    }
    {   // a zero-size buffer is never touched, not even for the NUL (a null pointer would crash here)
        Out o{nullptr, 0};
        o.fmt("%s", "anything");
        o.str("x", 1);
        assert(o.full && o.n == 0);
    }
    {   // one byte: only the empty text fits
        Buf b(1);
        Out o{b.mem, b.cap};
        o.fmt("%s", "");
        assert(!o.full);
        wholeText(b, o, "");
        o.fmt("a");
        assert(o.full);
        wholeText(b, o, "");
    }
    {   // JSON strings: quotes, backslashes and control characters are escaped
        Buf b(64);
        Out o{b.mem, b.cap};
        const char label[] = "a\"b\\c\x01\x7f";
        o.str(label, sizeof label - 1);
        assert(!o.full);
        wholeText(b, o, "\"a\\\"b\\\\c\\u0001\\u007f\"");
    }
    {   // a JSON string that does not fit: full, the text ends at its last whole escape, the guard is intact
        for (size_t cap = 1; cap <= 24; ++cap) {
            Buf b(cap);
            Out o{b.mem, b.cap};
            const char label[] = "\x01\x02\x03\"";
            o.str(label, sizeof label - 1);   // "\u0001\u0002\u0003\"" with quotes: 22 characters
            assert(o.full == (cap <= 22));
            assert(b.guardIntact() && o.n < cap && strlen(b.mem) == o.n);
            const std::string t(b.mem);
            assert(t == std::string("\"\\u0001\\u0002\\u0003\\\"\"").substr(0, t.size()));
            bool whole = false;   // the lengths after a whole piece: never half of an escape
            for (size_t len : {0, 1, 7, 13, 19, 21, 22}) whole = whole || t.size() == len;
            assert(whole);
        }
    }
    {   // CodeQL alert 4, the [mem-low] phase list: more names than the 96-byte buffer holds. The old loop added
        // snprintf's answer unchecked, so its size argument (96 - n) wrapped around once n passed 96.
        Buf b(96);
        Out o{b.mem, b.cap};
        const char* names[] = {"matter_start_with_a_long_name", "window_with_a_long_name", "commission_with_a_long_name",
                               "fourth_phase_name_long", "fifth"};
        for (const char* name : names) o.fmt("%s%s", o.n ? "," : "", name);
        assert(o.full);
        wholeText(b, o, "matter_start_with_a_long_name,window_with_a_long_name,commission_with_a_long_name");
    }
    {   // a [mem]-style line: many fields into a short buffer, every intermediate length below cap
        Buf b(40);
        Out o{b.mem, b.cap};
        size_t last = 0;
        for (unsigned i = 0; i < 100; ++i) {
            o.fmt(" f%u=%u", i, 4000000000u - i);
            assert(o.n >= last && o.n < b.cap && b.guardIntact());
            last = o.n;
        }
        assert(o.full && strlen(b.mem) == o.n);
    }
    printf("test_out: PASS\n");
    return 0;
}
