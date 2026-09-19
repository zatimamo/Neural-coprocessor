// ============================================================================
// MGPU NR worker - JSON writer and latency statistics. See nr_json.h.
// ============================================================================

#include "nr_json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace nr
{
    std::string Json::escape(const char *s)
    {
        std::string out;
        if (s == nullptr) return out;
        for (const unsigned char *p = (const unsigned char *)s; *p != 0; ++p)
        {
            const unsigned char c = *p;
            switch (c)
            {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04X", (unsigned)c);
                    out += buf;
                }
                else
                {
                    out += (char)c;
                }
                break;
            }
        }
        return out;
    }

    void Json::pre_value()
    {
        if (after_value_)
        {
            out_ += ',';
        }
        after_key_ = false;
    }

    Json &Json::begin_object()
    {
        pre_value();
        out_ += '{';
        Frame f;
        f.kind = '{';
        f.first = true;
        stack_.push_back(f);
        after_value_ = false;
        return *this;
    }

    Json &Json::end_object()
    {
        out_ += '}';
        if (!stack_.empty()) stack_.pop_back();
        after_key_ = false;
        after_value_ = true;
        return *this;
    }

    Json &Json::begin_array()
    {
        pre_value();
        out_ += '[';
        Frame f;
        f.kind = '[';
        f.first = true;
        stack_.push_back(f);
        after_value_ = false;
        return *this;
    }

    Json &Json::end_array()
    {
        out_ += ']';
        if (!stack_.empty()) stack_.pop_back();
        after_key_ = false;
        after_value_ = true;
        return *this;
    }

    Json &Json::key(const char *k)
    {
        if (after_value_)
        {
            out_ += ',';
        }
        out_ += '"';
        out_ += escape(k);
        out_ += "\":";
        after_key_ = true;
        after_value_ = false;
        return *this;
    }

    Json &Json::value_string(const char *v)
    {
        pre_value();
        out_ += '"';
        out_ += escape(v);
        out_ += '"';
        after_value_ = true;
        return *this;
    }

    Json &Json::value_u64(std::uint64_t v)
    {
        pre_value();
        char buf[32];
        std::snprintf(buf, sizeof buf, "%llu", (unsigned long long)v);
        out_ += buf;
        after_value_ = true;
        return *this;
    }

    Json &Json::value_i64(std::int64_t v)
    {
        pre_value();
        char buf[32];
        std::snprintf(buf, sizeof buf, "%lld", (long long)v);
        out_ += buf;
        after_value_ = true;
        return *this;
    }

    Json &Json::value_int(int v)
    {
        return value_i64((std::int64_t)v);
    }

    Json &Json::value_double(double v, int decimals)
    {
        pre_value();
        if (!(v == v) || v > 1.0e308 || v < -1.0e308)      // NaN or infinite
        {
            // JSON has no way to say "not a number". A measurement that came out
            // NaN is reported as null, which is a statement, rather than as a
            // number that happens to parse.
            out_ += "null";
            after_value_ = true;
            return *this;
        }
        if (decimals < 0) decimals = 0;
        if (decimals > 9) decimals = 9;
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
        out_ += buf;
        after_value_ = true;
        return *this;
    }

    Json &Json::value_bool(bool v)
    {
        pre_value();
        out_ += v ? "true" : "false";
        after_value_ = true;
        return *this;
    }

    Json &Json::value_null()
    {
        pre_value();
        out_ += "null";
        after_value_ = true;
        return *this;
    }

    Json &Json::kv_doubles(const char *k,
                           const std::vector<std::pair<std::string, double> > &v)
    {
        key(k);
        begin_object();
        for (std::size_t i = 0; i < v.size(); ++i)
        {
            key(v[i].first.c_str());
            value_double(v[i].second);
        }
        end_object();
        return *this;
    }

    Stats summarise(std::vector<double> samples)
    {
        Stats s;
        if (samples.empty()) return s;

        std::sort(samples.begin(), samples.end());
        s.n = (std::uint32_t)samples.size();
        s.min = samples.front();
        s.max = samples.back();

        double sum = 0.0;
        for (std::size_t i = 0; i < samples.size(); ++i) sum += samples[i];
        s.mean = sum / (double)samples.size();

        s.median = samples[(samples.size() - 1) / 2];            // lower median
        std::size_t rank = (std::size_t)std::ceil(0.95 * (double)samples.size());
        if (rank < 1) rank = 1;
        if (rank > samples.size()) rank = samples.size();
        s.p95 = samples[rank - 1];
        return s;
    }

    double us_to_ms(std::uint64_t us)
    {
        return (double)us / 1000.0;
    }

    bool write_text_file(const char *path, const std::string &text, std::string &err)
    {
        FILE *f = std::fopen(path, "wb");
        if (f == nullptr)
        {
            err = std::string("could not open ") + path + " for writing";
            return false;
        }
        const std::size_t wrote = std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
        if (wrote != text.size())
        {
            err = std::string("short write to ") + path;
            return false;
        }
        return true;
    }
}
