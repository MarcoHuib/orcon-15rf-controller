// dump_pages.cpp — print one built-in web page (index 0..6) for the CI JS syntax check.
#include <cstdlib>
#include "web_page.h"
#include <cstdio>
int main(int argc, char** argv) {
    const char* pages[] = { INDEX_HTML, LOG_HTML, WIDGET_HTML, DEBUG_HTML, FLOW_HTML, SETUP_HTML, UPDATE_HTML };
    int i = argc > 1 ? atoi(argv[1]) : -1;
    if (i < 0 || i > 6) return 2;
    fputs(pages[i], stdout);
    return 0;
}
