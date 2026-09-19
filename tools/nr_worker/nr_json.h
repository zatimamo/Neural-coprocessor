// ============================================================================
// MGPU NR worker - reporting: a small JSON writer and the latency statistics.
//
// WHY NOT A JSON LIBRARY
//     The deliverable is one machine-readable file written by a diagnostic whose
//     whole value is that it has no dependencies to drift. The writer below is
//     deliberately boring: it can emit objects, arrays, numbers, booleans and
//     escaped strings, and it cannot emit malformed JSON because it tracks
//     whether a value is expected next.
//
// PERCENTILES ARE NEAREST-RANK, ON PURPOSE
//     p95 of a sorted sample of n is element ceil(0.95 * n) - 1. With a handful
//     of frames that is the honest number; an interpolated percentile would
//     invent a latency between two measurements that was never observed.
// ============================================================================

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nr
{
    class Json
    {
    public:
        Json &begin_object();
        Json &end_object();
        Json &begin_array();
        Json &end_array();

        Json &key(const char *k);

        Json &value_string(const char *v);
        Json &value_u64(std::uint64_t v);
        Json &value_i64(std::int64_t v);
        Json &value_int(int v);
        Json &value_double(double v, int decimals = 3);
        Json &value_bool(bool v);
        Json &value_null();

        //: Convenience: key + value in one call.
        Json &kv(const char *k, const char *v) { return key(k).value_string(v); }
        Json &kv(const char *k, std::uint64_t v) { return key(k).value_u64(v); }
        Json &kv(const char *k, std::int64_t v) { return key(k).value_i64(v); }
        Json &kv(const char *k, int v) { return key(k).value_int(v); }
        Json &kv(const char *k, double v) { return key(k).value_double(v); }
        Json &kv(const char *k, bool v) { return key(k).value_bool(v); }
        Json &kv_null(const char *k) { return key(k).value_null(); }

        //: An object of doubles, e.g. "stages": { "ingress": 0.42, ... }.
        Json &kv_doubles(const char *k, const std::vector<std::pair<std::string, double> > &v);

        const std::string &str() const { return out_; }

        //: True when every container that was opened has been closed. The
        //: benchmark refuses to write its report if this is false, so a truncated
        //: document can never be mistaken for a result.
        bool well_formed() const { return stack_.empty(); }

        static std::string escape(const char *s);

    private:
        //: Write the separator a value needs, given what came before it in the
        //: container it is being written into.
        void pre_value();

        struct Frame
        {
            char   kind;      // '{' or '['
            bool   first;     // nothing written inside it yet
        };

        std::string        out_;
        std::vector<Frame> stack_;
        bool               after_key_ = false;    // a key was written, its value is due
        bool               after_value_ = false;  // a value was written and is complete
    };

    //: Latency statistics for one stage. All values are milliseconds.
    struct Stats
    {
        std::uint32_t n = 0;
        double min = 0.0;
        double median = 0.0;
        double p95 = 0.0;
        double max = 0.0;
        double mean = 0.0;
    };

    //: Nearest-rank statistics over the samples. An empty sample yields a Stats
    //: with n == 0 and every value 0, which the report renders as null rather
    //: than as a zero-latency measurement.
    Stats summarise(std::vector<double> samples);

    //: Microseconds -> milliseconds.
    double us_to_ms(std::uint64_t us);

    //: Write a text file, LF endings. Returns false and fills err on failure.
    bool write_text_file(const char *path, const std::string &text, std::string &err);
}
