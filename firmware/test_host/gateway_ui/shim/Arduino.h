// Minimo "Arduino.h" per provare DhGatewayUi sul PC: solo String (stessa interfaccia che usa il codice).
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
class String {
public:
    std::string s;
    String() {}
    String(const char* c) : s(c ? c : "") {}
    String(const std::string& c) : s(c) {}
    template <typename T, typename = typename std::enable_if<std::is_integral<T>::value>::type>
    String(T v) : s(std::to_string(v)) {}
    unsigned length() const { return (unsigned)s.size(); }
    const char* c_str() const { return s.c_str(); }
    int indexOf(char c) const { auto p = s.find(c); return p == std::string::npos ? -1 : (int)p; }
    String substring(unsigned from) const { return String(from <= s.size() ? s.substr(from) : std::string()); }
    String substring(unsigned from, unsigned to) const { return String(from <= s.size() ? s.substr(from, to - from) : std::string()); }
    String& operator+=(const String& o) { s += o.s; return *this; }
    String& operator+=(const char* c) { s += c; return *this; }
    bool operator==(const String& o) const { return s == o.s; }
    bool operator!=(const String& o) const { return s != o.s; }
    bool operator<(const String& o) const { return s < o.s; }
};
inline String operator+(const String& a, const String& b) { return String(a.s + b.s); }
inline String operator+(const String& a, const char* b) { return String(a.s + b); }
inline String operator+(const char* a, const String& b) { return String(std::string(a) + b.s); }
template <typename T, typename = typename std::enable_if<std::is_integral<T>::value>::type>
String operator+(const String& a, T v) { return String(a.s + std::to_string(v)); }
