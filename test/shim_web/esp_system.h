#pragma once
#include <cstring>
inline void esp_fill_random(void* b, size_t n) { memset(b, 0xA5, n); }   // token = "a5a5..."
