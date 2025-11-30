#pragma once

#include <cstdint>

namespace FLog
{
    // Toggled at runtime in production builds; kept simple here for tools.
    static bool Asserts = false;
}

#define LOGGROUP(Name) \
    namespace FLog { static bool Name = false; }

#define LOGVARIABLE(Name, DefaultValue) \
    namespace FLog { static std::int32_t Name = static_cast<std::int32_t>(DefaultValue); }

#define LOGVARIABLEBOOL(Name, DefaultValue) LOGVARIABLE(Name, DefaultValue)
#define LOGVARIABLEINT(Name, DefaultValue) LOGVARIABLE(Name, DefaultValue)
#define LOGVARIABLEFLOAT(Name, DefaultValue) \
    namespace FLog { static float Name = static_cast<float>(DefaultValue); }

#define DFLOG(...)       do { } while (false)
#define DFLOG1F(...)     do { } while (false)
#define DFLOG2F(...)     do { } while (false)
#define DFLOG3F(...)     do { } while (false)
#define FASTLOG(...)     do { } while (false)
#define FASTLOG1F(...)   do { } while (false)
#define FASTLOG2F(...)   do { } while (false)
#define FASTLOG3F(...)   do { } while (false)
#define FASTLOGS(...)    do { } while (false)
#define FASTLOGCOUNT(...) do { } while (false)
