#pragma once

#include <cstdint>

namespace cradle {
namespace modules { class ModuleManager; }
}

namespace cradle::config
{
    class ConfigManager
    {
    public:
        static void initialize(modules::ModuleManager &manager);
    static void tick(modules::ModuleManager &manager, bool force = false);
        static void mark_dirty();
        static void save_now(modules::ModuleManager &manager);
        static void load_now(modules::ModuleManager &manager);
        static void reset_to_defaults(modules::ModuleManager &manager);

    private:
    static bool load(modules::ModuleManager &manager);
    static void save(modules::ModuleManager &manager);
    };

    void mark_dirty();
}
