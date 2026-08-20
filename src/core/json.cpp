// json.cpp — JSON emission.
#include "lr/json.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <unistd.h>

namespace lr::json {

std::string escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    o += buf;
                } else {
                    o += c;
                }
        }
    }
    return o;
}

std::string num(double v, int prec) {
    // A non-finite value becomes null rather than the token "nan", which no JSON
    // parser accepts. Failing at the consumer is better than a silent string.
    if (!std::isfinite(v)) return "null";
    // `prec` counts digits after the decimal point, not significant digits. The
    // difference matters: a rate of 300000 pps written with three significant
    // digits becomes 3e+05, and every consumer downstream then reads a number
    // that is wrong by up to half a percent for no reason.
    if (prec < 0) prec = 0;
    if (prec > 12) prec = 12;
    char fmt[16], buf[512];
    std::snprintf(fmt, sizeof(fmt), "%%.%df", prec);
    std::snprintf(buf, sizeof(buf), fmt, v);
    // Trim trailing zeros so the file stays readable, but always leave a digit
    // after the point when there is one, because "3." does not parse.
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        size_t last = s.find_last_not_of('0');
        if (last != std::string::npos && s[last] == '.') last++;
        s.erase(last + 1);
    }
    return s;
}

void Writer::sep() {
    if (after_key_) { after_key_ = false; return; }
    if (need_sep_) out_ << ",";
    if (!in_arr_.empty()) {
        out_ << "\n";
        for (size_t i = 0; i < in_arr_.size(); ++i)
            out_ << std::string(step_, ' ');
    }
    need_sep_ = true;
}

Writer& Writer::obj_open() { sep(); out_ << "{"; in_arr_.push_back(false); need_sep_ = false; return *this; }
Writer& Writer::arr_open() { sep(); out_ << "["; in_arr_.push_back(true);  need_sep_ = false; return *this; }

Writer& Writer::obj_close() {
    in_arr_.pop_back();
    out_ << "\n";
    for (size_t i = 0; i < in_arr_.size(); ++i) out_ << std::string(step_, ' ');
    out_ << "}";
    need_sep_ = true;
    return *this;
}

Writer& Writer::arr_close() {
    in_arr_.pop_back();
    out_ << "\n";
    for (size_t i = 0; i < in_arr_.size(); ++i) out_ << std::string(step_, ' ');
    out_ << "]";
    need_sep_ = true;
    return *this;
}

Writer& Writer::key(const std::string& k) {
    sep();
    out_ << "\"" << escape(k) << "\": ";
    after_key_ = true;
    return *this;
}

Writer& Writer::val(const std::string& s) { sep(); out_ << "\"" << escape(s) << "\""; return *this; }
Writer& Writer::val(double v, int prec)   { sep(); out_ << num(v, prec);              return *this; }
Writer& Writer::val_ll(long long v)       { sep(); out_ << v;                          return *this; }
Writer& Writer::val(bool v)               { sep(); out_ << (v ? "true" : "false");     return *this; }
Writer& Writer::null()                    { sep(); out_ << "null";                     return *this; }
Writer& Writer::raw(const std::string& r) { sep(); out_ << r;                          return *this; }

Writer& Writer::kv_strs(const std::string& k, const std::vector<std::string>& v) {
    key(k).arr_open();
    for (const auto& s : v) val(s);
    return arr_close();
}

Writer& Writer::kv_nums(const std::string& k, const std::vector<double>& v, int prec) {
    key(k).arr_open();
    for (double d : v) val(d, prec);
    return arr_close();
}

bool Writer::write_file(const std::string& path) const {
    // Write to a temporary and rename. A run that dies mid-write must leave the
    // previous file intact rather than a half-written one that parses.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << out_.str() << "\n";
        if (!f) return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

} // namespace lr::json
