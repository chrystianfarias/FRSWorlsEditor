#pragma once

// Minimal JSON DOM for the golden files (tools/harness/golden in NFSU2Unity). The whole file is loaded
// into one buffer and strings/numbers are views into it (the goldens have no escaped characters except
// in a few descriptive fields, which are kept raw). Large files (50 MB) parse in well under a second.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace golden {

struct JVal {
    enum Type : uint8_t { TNull, TBool, TNum, TStr, TArr, TObj };
    Type t = TNull;
    bool b = false;
    std::string_view s;                 // Str: contents (raw), Num: text
    std::vector<JVal> a;                // Arr elements / Obj values
    std::vector<std::string_view> k;    // Obj keys

    bool IsNum() const { return t == TNum; }
    bool IsStr() const { return t == TStr; }
    bool IsObj() const { return t == TObj; }
    bool IsArr() const { return t == TArr; }

    const JVal* Get(std::string_view key) const {
        if (t != TObj) return nullptr;
        for (size_t i = 0; i < k.size(); i++)
            if (k[i] == key) return &a[i];
        return nullptr;
    }
    const JVal& operator[](std::string_view key) const {
        const JVal* v = Get(key);
        if (!v) throw std::runtime_error("json: missing property '" + std::string(key) + "'");
        return *v;
    }
    const JVal& operator[](size_t i) const {
        if (t != TArr || i >= a.size()) throw std::runtime_error("json: index out of range");
        return a[i];
    }
    const JVal& operator[](int i) const { return (*this)[static_cast<size_t>(i)]; }
    size_t Size() const { return a.size(); }

    int64_t Int() const {
        if (t != TNum) throw std::runtime_error("json: not a number");
        return std::strtoll(std::string(s).c_str(), nullptr, 10);
    }
    int I32() const { return static_cast<int>(Int()); }
    double Dbl() const {
        if (t != TNum) throw std::runtime_error("json: not a number");
        return std::strtod(std::string(s).c_str(), nullptr);
    }
    std::string Str() const {
        if (t != TStr) throw std::runtime_error("json: not a string");
        return std::string(s);
    }
};

class JDoc {
public:
    std::string Text;
    JVal Root;

    void Parse() {
        p_ = Text.data();
        e_ = Text.data() + Text.size();
        Ws();
        Value(Root);
    }

private:
    const char* p_ = nullptr;
    const char* e_ = nullptr;

    [[noreturn]] void Fail(const char* m) { throw std::runtime_error(std::string("json: ") + m); }
    void Ws() { while (p_ < e_ && (*p_ == ' ' || *p_ == '\n' || *p_ == '\r' || *p_ == '\t')) p_++; }

    std::string_view String() {
        if (*p_ != '"') Fail("expected string");
        const char* s = ++p_;
        while (p_ < e_ && *p_ != '"') { if (*p_ == '\\') p_++; p_++; }
        if (p_ >= e_) Fail("unterminated string");
        std::string_view v(s, static_cast<size_t>(p_ - s));
        p_++;
        return v;
    }

    void Value(JVal& v) {
        if (p_ >= e_) Fail("unexpected end");
        char c = *p_;
        if (c == '{') {
            v.t = JVal::TObj; p_++; Ws();
            if (*p_ == '}') { p_++; return; }
            for (;;) {
                Ws();
                v.k.push_back(String());
                Ws(); if (*p_ != ':') Fail("expected ':'"); p_++; Ws();
                v.a.emplace_back();
                Value(v.a.back());
                Ws();
                if (*p_ == ',') { p_++; continue; }
                if (*p_ == '}') { p_++; return; }
                Fail("expected ',' or '}'");
            }
        } else if (c == '[') {
            v.t = JVal::TArr; p_++; Ws();
            if (*p_ == ']') { p_++; return; }
            for (;;) {
                Ws();
                v.a.emplace_back();
                Value(v.a.back());
                Ws();
                if (*p_ == ',') { p_++; continue; }
                if (*p_ == ']') { p_++; return; }
                Fail("expected ',' or ']'");
            }
        } else if (c == '"') {
            v.t = JVal::TStr; v.s = String();
        } else if (c == 't') { v.t = JVal::TBool; v.b = true; p_ += 4; }
        else if (c == 'f') { v.t = JVal::TBool; v.b = false; p_ += 5; }
        else if (c == 'n') { v.t = JVal::TNull; p_ += 4; }
        else {
            const char* s = p_;
            while (p_ < e_ && (std::strchr("+-0123456789.eE", *p_) != nullptr)) p_++;
            if (p_ == s) Fail("bad value");
            v.t = JVal::TNum; v.s = std::string_view(s, static_cast<size_t>(p_ - s));
        }
    }
};

} // namespace golden
