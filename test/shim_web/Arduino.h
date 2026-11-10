// Host shim for setup_web.cpp: test/shim/Arduino.h plus the String/PROGMEM bits it uses.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
unsigned long millis();
void delay(unsigned long);
#define PROGMEM
typedef const char* PGM_P;
class String {
public:
    String(const char* c = "") : s_(c ? c : "") {}
    String(const std::string& s) : s_(s) {}
    const char* c_str() const { return s_.c_str(); }
    size_t length() const { return s_.size(); }
    bool concat(const char* b) { s_ += b; return true; }   // ArduinoJson writer
    int indexOf(const char* n) const { size_t p = s_.find(n); return p == std::string::npos ? -1 : int(p); }
private:
    std::string s_;
};
