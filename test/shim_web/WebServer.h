// Fake WebServer: records routes and the last response. Only collected headers are
// visible through header(), exactly like the ESP32 WebServer.
#pragma once
#include <Arduino.h>
#include <strings.h>
#include <functional>
#include <map>
#include <string>
#include <vector>
enum HTTPMethod { HTTP_ANY, HTTP_GET, HTTP_POST };
class WebServer {
public:
    std::map<std::string, std::function<void()>> routes;          // "GET /path" -> handler
    std::vector<std::string> collected;
    std::map<std::string, std::string> req, sent;                  // request / response headers
    std::string host, body, type, out;
    int code = 0;
    void on(const char* uri, HTTPMethod m, std::function<void()> f) { routes[(m == HTTP_GET ? "GET " : "POST ") + std::string(uri)] = f; }
    void collectHeaders(const char* h[], size_t n) { collected.assign(h, h + n); }
    String header(const char* n) {
        for (auto& c : collected)
            if (!strcasecmp(c.c_str(), n)) { auto it = req.find(c); return it == req.end() ? String("") : String(it->second); }
        return String("");
    }
    String hostHeader() { return String(host); }
    String arg(const char* n) { return std::string(n) == "plain" ? String(body) : String(""); }
    void sendHeader(const char* k, const char* v) { sent[k] = v; }
    void send(int c, const char* t, const char* b) { code = c; type = t; out = b; }
    void send_P(int c, PGM_P t, PGM_P b, size_t n) { code = c; type = t; out.assign(b, n); }
};
