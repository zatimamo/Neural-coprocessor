// ============================================================================
// MGPU Bridge - APPID_NS implementation. See appid_ns.hpp.
// ============================================================================
// This file holds ONE number and the five log lines that report it. It creates
// no object, resolves no entry point, loads no module, writes no parameter and
// calls nothing in NGX or NVAPI. The call sites in gpu1_context.cpp read the
// number from here instead of writing a literal, so that the value a session is
// initialised with and the value the log states cannot drift apart.
// ============================================================================

#include "appid_ns.hpp"

#include <cstdio>

#include "diag.hpp"

namespace mgpu::appidns
{
#ifdef MGPU_APPID_NS
namespace
{
    // The value NeuralScreen's working Reserved18 path passes to BOTH the core
    // Init and the snippet Init_Ext. Seven significant hex digits: 0x1000000.
    const unsigned long long kReserved18AppId = 0x1000000ULL;

    const char *const kVariant = "APPID_NS";
}
#else
namespace
{
    // Today's value, unchanged. The control arm exists so that this one number
    // is the ONLY difference between the two builds.
    const unsigned long long kReserved18AppId = 0ULL;

    const char *const kVariant = "CONTROL";
}
#endif

unsigned long long reserved18_app_id()
{
    return kReserved18AppId;
}

namespace
{
    // One formatter for all five lines, so the width and the padding can never
    // differ between them. %016llX is what makes the expected line
    // "[APPID] P1 core app_id=0x0000000001000000" the same length in both arms,
    // which is what lets a reader diff two logs by eye.
    void log_site(const char *site)
    {
        char l[160];
        std::snprintf(l, sizeof l, "[APPID] %s app_id=0x%016llX", site, kReserved18AppId);
        mgpu::diag::info(l);
    }
}

void log_variant()
{
    char l[256];
    std::snprintf(l, sizeof l,
                  "[APPID] variant=%s reserved18 app_id=0x%016llX - applies to the P1.0c and "
                  "P4.1 Reserved18 sessions, core and snippet. The C2-SR session is NOT a "
                  "Reserved18 session and is unchanged at 0x0000000000000000.",
                  kVariant, kReserved18AppId);
    mgpu::diag::info(l);
}

void log_p1_core()    { log_site("P1 core"); }
void log_p1_snippet() { log_site("P1 snippet"); }
void log_p4_core()    { log_site("P4 core"); }
void log_p4_snippet() { log_site("P4 snippet"); }
}
