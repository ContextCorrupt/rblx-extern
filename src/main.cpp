#include <spdlog/spdlog.h>
#include <chrono>
#include <thread>
#include "overlay/overlay.hpp"
#include "util/memory/memory.hpp"
#include "util/engine/offsets.hpp"
#include "util/engine/datamodel/datamodel.hpp"
#include "cache/player_cache.hpp"
#include "modules/module_manager.hpp"
#include "modules/esp/esp_module.hpp"
#include "modules/esp/esp_visuals_module.hpp"
#include "modules/silent_aim/silent_aim_module.hpp"
#include "modules/anti_aim/anti_aim_module.hpp"
#include "modules/aimbot/aimbot_module.hpp"
#include "modules/triggerbot/triggerbot_module.hpp"
#include "modules/friends/friends_module.hpp"
#include "modules/profiling/profiling_module.hpp"
#include "config/config_manager.hpp"

namespace
{
    DWORD wait_for_roblox(bool &waited_for_launch)
    {
        constexpr auto kPollDelay = std::chrono::milliseconds(500);
        waited_for_launch = false;

        while (true)
        {
            DWORD pid = cradle::memory::FindProcess("RobloxPlayerBeta.exe");
            if (pid != 0)
                return pid;

            if (!waited_for_launch)
            {
                spdlog::info("RobloxPlayerBeta.exe not running; waiting for it to launch...");
                waited_for_launch = true;
            }

            std::this_thread::sleep_for(kPollDelay);
        }
    }

    bool try_read_walkspeed_check(bool &value_out)
    {
        using namespace cradle::engine;

        DataModel dm = DataModel::get_instance();
        if (!dm.is_valid())
            return false;

        Instance players = dm.get_players();
        if (!players.is_valid())
            return false;

        Instance local_player = players.get_local_player();
        if (!local_player.is_valid())
            return false;

        Instance character = local_player.get_character();
        if (!character.is_valid())
            return false;

        Instance humanoid = character.find_first_child_of_class("Humanoid");
        if (!humanoid.is_valid())
            return false;

        std::uint8_t raw_value = cradle::memory::read<std::uint8_t>(humanoid.address + Offsets::Humanoid::WalkspeedCheck);
        value_out = raw_value != 0;
        return true;
    }

    void announce_walkspeed_check()
    {
        constexpr int kMaxAttempts = 20;
        constexpr auto kRetryDelay = std::chrono::milliseconds(100);

        bool value = false;
        for (int attempt = 0; attempt < kMaxAttempts; ++attempt)
        {
            if (try_read_walkspeed_check(value))
            {
                spdlog::info("Humanoid WalkspeedCheck value: {} (byte {})", value ? "enabled" : "disabled", value ? 1 : 0);
                return;
            }
            std::this_thread::sleep_for(kRetryDelay);
        }

        spdlog::warn("Humanoid WalkspeedCheck value unavailable (local humanoid not ready)");
    }
}

int main()
{
    bool waited_for_launch = false;
    cradle::memory::processPid = wait_for_roblox(waited_for_launch);

    if (waited_for_launch)
    {
        spdlog::info("Roblox detected (pid: {}); waiting 5 seconds for it to finish loading...", cradle::memory::processPid);
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    else
    {
        spdlog::info("Roblox already running (pid: {}); attaching immediately.", cradle::memory::processPid);
    }

    cradle::memory::EnsureSyscallInit();
    cradle::memory::baseAddress = cradle::memory::GetProcessBase();
    spdlog::info("attached to roblox (pid: {}, base: 0x{:X})", cradle::memory::processPid, cradle::memory::baseAddress);
    announce_walkspeed_check();

    cradle::overlay::Overlay overlay;
    if (!overlay.initialize())
    {
        spdlog::error("overlay initialization failed");
        return 1;
    }

    auto &module_manager = cradle::modules::ModuleManager::get_instance();
    auto esp_module = std::make_unique<cradle::modules::ESPModule>();
    auto friends_module = std::make_unique<cradle::modules::FriendsModule>();
    auto esp_visuals_module = std::make_unique<cradle::modules::ESPVisualsModule>();
    auto aimbot_module = std::make_unique<cradle::modules::AimbotModule>();
    auto triggerbot_module = std::make_unique<cradle::modules::TriggerbotModule>();
    auto silent_aim_module = std::make_unique<cradle::modules::SilentAimModule>();
    auto anti_aim_module = std::make_unique<cradle::modules::AntiAimModule>();
    auto profiling_module = std::make_unique<cradle::modules::ProfilingModule>();
    module_manager.register_module(std::move(esp_module));
    module_manager.register_module(std::move(friends_module));
    module_manager.register_module(std::move(esp_visuals_module));
    module_manager.register_module(std::move(aimbot_module));
    module_manager.register_module(std::move(triggerbot_module));
    module_manager.register_module(std::move(silent_aim_module));
    module_manager.register_module(std::move(anti_aim_module));
    module_manager.register_module(std::move(profiling_module));
    module_manager.update_keybinds();
    cradle::config::ConfigManager::initialize(module_manager);
    spdlog::info("Mossad.is modules initialized");

    spdlog::info("overlay initialized - press delete to toggle menu");

    while (overlay.isRunning())
    {
        overlay.render();
    }

    cradle::config::ConfigManager::tick(module_manager, true);
    overlay.cleanup();
    return 0;
}