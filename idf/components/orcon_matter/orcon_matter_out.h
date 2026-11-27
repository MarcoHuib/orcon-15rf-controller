#pragma once
// Text into a fixed buffer, with every vsnprintf answer checked before it moves the length. A negative answer (an
// encoding error) or a piece that does not fit marks the text full and drops that piece: the buffer keeps its
// earlier, whole pieces, NUL-terminated, and later pieces are ignored. Nothing is written at or past b + cap, and
// n stays below cap. A full JSON answer becomes an error, never broken JSON (orcon_matter_pairing.cpp send()).
// Only the C library: host tests in test/matter/test_out.cpp.
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

namespace orcon_matter {
struct Out {
    char* b;
    size_t cap, n = 0;
    bool full = false;
    Out(char* buf, size_t size) : b(buf), cap(size) {
        if (cap) b[0] = '\0';
    }
    __attribute__((format(printf, 2, 3))) void fmt(const char* f, ...) {
        if (full || n >= cap) { full = true; return; }   // n >= cap only for a zero-size buffer: nothing to write
        va_list ap;
        va_start(ap, f);
        const int k = vsnprintf(b + n, cap - n, f, ap);
        va_end(ap);
        settle(k);
    }
    void settle(int k) {   // vsnprintf's answer for the piece it just wrote at b + n
        if (k >= 0 && size_t(k) < cap - n) { n += size_t(k); return; }
        full = true;
        b[n] = '\0';
    }
    void str(const char* s, size_t len) {   // a JSON string; controller labels are a controller's free text
        fmt("\"");
        for (size_t i = 0; i < len && !full; ++i) {
            const unsigned char c = (unsigned char)s[i];
            if (c == '"' || c == '\\') fmt("\\%c", c);
            else if (c < 0x20 || c == 0x7F) fmt("\\u%04x", c);
            else fmt("%c", c);
        }
        fmt("\"");
    }
};
}  // namespace orcon_matter
