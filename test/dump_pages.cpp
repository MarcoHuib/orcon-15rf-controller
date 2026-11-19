// dump_pages.cpp — print one built-in legacy web page (index 0..1) for the JS syntax check.
#include <cstdlib>
#include "web_page.h"
#include <cstdio>
int main(int argc, char** argv) {
    const char* pages[] = { LOG_HTML, UPDATE_HTML };
    int i = argc > 1 ? atoi(argv[1]) : -1;
    if (i < 0 || i > 1) return 2;
    fputs(pages[i], stdout);
    return 0;
}
