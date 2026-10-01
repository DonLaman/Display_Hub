// Minimo "Arduino.h" per provare portal_util.h sul PC: solo String.
#pragma once
#include <string>
#define F(x) x
class String {
public:
    std::string s;
    String() {}
    String(const char* c) : s(c ? c : "") {}
    String(int v) : s(std::to_string(v)) {}
    unsigned length() const { return (unsigned)s.size(); }
    char operator[](unsigned i) const { return s[i]; }
    void reserve(unsigned n) { s.reserve(n); }
    const char* c_str() const { return s.c_str(); }
    String& operator+=(const String& o) { s += o.s; return *this; }
    String& operator+=(const char* c) { s += c; return *this; }
    String& operator+=(char c) { s += c; return *this; }
};
inline String operator+(const String& a, const String& b) { String r; r.s = a.s + b.s; return r; }
