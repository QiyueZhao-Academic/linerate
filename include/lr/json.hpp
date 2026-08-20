// json.hpp — a writer, not a parser.
//
// The harness only ever emits JSON; everything that reads it is Python. Keeping
// this to a writer avoids a dependency and keeps the emitted shape in one place,
// which is what makes results.json a stable contract between the C++ and the
// report (see docs/schema.md and tools/check_wiring.py).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace lr::json {

std::string escape(const std::string& s);

// Fixed-precision number formatting. Non-finite values are emitted as null,
// because a NaN silently becoming the token "nan" would break every consumer
// downstream and would do it quietly.
std::string num(double v, int prec = 6);

class Writer {
public:
    explicit Writer(int indent_step = 2) : step_(indent_step) {}

    Writer& obj_open();
    Writer& obj_close();
    Writer& arr_open();
    Writer& arr_close();
    Writer& key(const std::string& k);

    Writer& val(const std::string& s);
    Writer& val(const char* s) { return val(std::string(s)); }
    Writer& val(double v, int prec = 6);
    Writer& val(bool v);
    // One template rather than an overload per width: size_t and uint64_t are
    // the same type on some platforms and different on others, and writing both
    // out by hand is an ambiguity waiting to happen.
    template <class T, class = std::enable_if_t<std::is_integral<T>::value &&
                                                !std::is_same<T, bool>::value>>
    Writer& val(T v) { return val_ll(static_cast<long long>(v)); }

    Writer& null();
    Writer& raw(const std::string& r);          // pre-rendered fragment

    Writer& kv(const std::string& k, const std::string& v) { return key(k).val(v); }
    Writer& kv(const std::string& k, const char* v)        { return key(k).val(v); }
    Writer& kv(const std::string& k, double v, int p = 6)  { return key(k).val(v, p); }
    Writer& kv(const std::string& k, bool v)               { return key(k).val(v); }
    template <class T, class = std::enable_if_t<std::is_integral<T>::value &&
                                                !std::is_same<T, bool>::value>>
    Writer& kv(const std::string& k, T v) { return key(k).val_ll(static_cast<long long>(v)); }

    Writer& kv_strs(const std::string& k, const std::vector<std::string>& v);
    Writer& kv_nums(const std::string& k, const std::vector<double>& v, int prec = 6);

    std::string str() const { return out_.str(); }
    // Writes atomically: a truncated run must be visibly truncated, never a
    // half-written file that parses.
    bool write_file(const std::string& path) const;

private:
    Writer& val_ll(long long v);
    void sep();
    std::ostringstream out_;
    std::vector<bool>  in_arr_;
    int  step_;
    bool need_sep_  = false;
    bool after_key_ = false;
};

} // namespace lr::json
