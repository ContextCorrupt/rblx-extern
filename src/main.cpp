#include <spdlog/spdlog.h>
#include <chrono>
#include <thread>
#include <atomic>
#include <algorithm>
#include <string>
#include <iostream>
#include <fmt/format.h>
#include <windows.h>
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
#include "util/security/hwid.hpp"

namespace
{
    constexpr std::size_t kRobloxInlineStringThreshold = 16;
    constexpr std::size_t kRobloxMaxStringLength = 512;

    std::string read_ascii_string(uintptr_t address, std::size_t max_len)
    {
        if (!cradle::memory::IsValid(address))
            return {};

        std::string result;
        result.reserve(std::min<std::size_t>(max_len, 32));

        for (std::size_t i = 0; i < max_len; ++i)
        {
            char c = cradle::memory::read<char>(address + i);
            if (c == '\0')
                break;
            result.push_back(c);
        }

        return result;
    }

    std::string read_roblox_string(uintptr_t string_ptr)
    {
        if (!cradle::memory::IsValid(string_ptr))
            return {};

        auto safe_length = [&](uintptr_t offset) -> std::uint32_t {
            if (!cradle::memory::IsValid(string_ptr + offset))
                return 0;
            return cradle::memory::read<std::uint32_t>(string_ptr + offset);
        };

        std::uint32_t declared_length = safe_length(0x10);
        if (declared_length == 0 || declared_length > kRobloxMaxStringLength)
        {
            std::uint32_t alt = safe_length(0x18);
            if (alt > 0 && alt <= kRobloxMaxStringLength)
                declared_length = alt;
        }

        std::size_t max_read = declared_length > 0 ? std::min<std::size_t>(declared_length + 1, kRobloxMaxStringLength) : kRobloxMaxStringLength;
        bool prefer_inline = declared_length > 0 && declared_length < kRobloxInlineStringThreshold;

        if (prefer_inline)
        {
            auto inline_value = read_ascii_string(string_ptr, max_read);
            if (!inline_value.empty())
                return inline_value;
        }

        auto heap_ptr = cradle::memory::read<uintptr_t>(string_ptr);
        if (cradle::memory::IsValid(heap_ptr))
        {
            auto heap_value = read_ascii_string(heap_ptr, max_read);
            if (!heap_value.empty())
                return heap_value;
        }

        if (!prefer_inline)
        {
            auto fallback = read_ascii_string(string_ptr, kRobloxInlineStringThreshold);
            if (!fallback.empty())
                return fallback;
        }

        return {};
    }

    DWORD wait_for_roblox(bool &waited_for_launch, std::atomic<bool> &cancel_flag)
    {
        constexpr auto kPollDelay = std::chrono::milliseconds(500);
        waited_for_launch = false;

        while (!cancel_flag.load(std::memory_order_relaxed))
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

        return 0;
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

    void LogLightingSnapshot()
    {
        using namespace cradle::engine;

        DataModel dm = DataModel::get_instance();
        if (!dm.is_valid())
        {
            std::cout << "[lighting] DataModel unavailable; skipping lighting offsets dump" << std::endl;
            return;
        }

        Instance lighting = dm.find_first_child_of_class("Lighting");
        if (!lighting.is_valid())
        {
            std::cout << "[lighting] Lighting service missing; cannot dump offsets" << std::endl;
            return;
        }

        auto log_float = [&](const char *label, uintptr_t offset) {
            float value = cradle::memory::read<float>(lighting.address + offset);
            std::cout << fmt::format("[lighting] {} = {:.3f}", label, value) << std::endl;
        };

        auto log_color = [&](const char *label, uintptr_t offset) {
            vector3 color = cradle::memory::read<vector3>(lighting.address + offset);
            std::cout << fmt::format("[lighting] {} = ({:.3f}, {:.3f}, {:.3f})", label, color.X, color.Y, color.Z) << std::endl;
        };

        log_float("Brightness", Offsets::Lighting::Brightness);
        log_color("Ambient", Offsets::Lighting::Ambient);
        log_color("ColorShift_Bottom", Offsets::Lighting::ColorShift_Bottom);
        log_color("ColorShift_Top", Offsets::Lighting::ColorShift_Top);
        log_color("FogColor", Offsets::Lighting::FogColor);

        Instance sky = lighting.find_first_child_of_class("Sky");
        if (!sky.is_valid())
        {
            auto skies = lighting.find_descendants_of_class("Sky");
            if (!skies.empty())
                sky = skies.front();
        }

        if (!sky.is_valid())
        {
            std::cout << "[lighting] Sky instance missing; skybox offsets skipped" << std::endl;
            return;
        }

        auto log_skybox = [&](const char *label, uintptr_t offset) {
            uintptr_t str_ptr = cradle::memory::read<uintptr_t>(sky.address + offset);
            std::string value = read_roblox_string(str_ptr);
            if (value.empty())
                value = "<empty>";
            std::cout << fmt::format("[sky] {} = {}", label, value) << std::endl;
        };

        log_skybox("SkyboxBk", Offsets::Sky::SkyboxBk);
        log_skybox("SkyboxDn", Offsets::Sky::SkyboxDn);
        log_skybox("SkyboxFt", Offsets::Sky::SkyboxFt);
        log_skybox("SkyboxLf", Offsets::Sky::SkyboxLf);
    }

    bool EnforceHardwareAuthorization()
    {
        auto whitelist = cradle::security::LoadGpuWhitelist();
        auto validation = cradle::security::ValidateGpuWhitelist(whitelist);
        if (validation.authorized)
        {
            spdlog::info("Authorized GPU HWID detected: {}", validation.detected_id);
            return true;
        }

        std::string message = validation.message.empty() ? "GPU HWID authorization failed" : validation.message;
        if (!validation.detected_id.empty())
        {
            message += "\nDetected GPU: ";
            message += validation.detected_id;
        }

        auto whitelist_path = cradle::security::GetWhitelistFilePath();
        message += "\nWhitelist file: ";
        message += whitelist_path.u8string();
        message += "\nAdd the GPU's PNPDeviceID to authorize this system.";

        spdlog::error("HWID check failed: {}", message);
        MessageBoxA(nullptr, message.c_str(), "Cradle loader", MB_OK | MB_ICONERROR);
        return false;
    }
}

int main()
{
    if (!EnforceHardwareAuthorization())
        return 1;

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

    cradle::overlay::Overlay::set_runtime_ready(false);
    cradle::overlay::Overlay::set_loading_stage(cradle::overlay::Overlay::LoadingStage::WaitingForRoblox,
                                               "Waiting for RobloxPlayerBeta.exe...");

    std::atomic<bool> shutdown_requested{false};
    std::thread attach_thread([&]() {
        bool waited_for_launch = false;
        DWORD pid = wait_for_roblox(waited_for_launch, shutdown_requested);
        if (pid == 0)
        {
            cradle::overlay::Overlay::set_loading_stage(cradle::overlay::Overlay::LoadingStage::Failed,
                                                        "Shutting down...");
            return;
        }

        cradle::memory::processPid = pid;
    cradle::overlay::Overlay::set_loading_stage(cradle::overlay::Overlay::LoadingStage::Attaching,
                            "Roblox detected - preparing memory");

        if (waited_for_launch)
        {
            spdlog::info("Roblox detected (pid: {}); waiting 5 seconds for it to finish loading...", pid);
            cradle::overlay::Overlay::set_loading_stage(cradle::overlay::Overlay::LoadingStage::Attaching,
                                                        "Roblox detected - warming up client");
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
        else
        {
            spdlog::info("Roblox already running (pid: {}); attaching immediately.", pid);
        }

        cradle::overlay::Overlay::set_loading_stage(cradle::overlay::Overlay::LoadingStage::Initializing,
                                                    "Attaching to Roblox process");
        cradle::memory::EnsureSyscallInit();
        cradle::memory::baseAddress = cradle::memory::GetProcessBase();
        spdlog::info("attached to roblox (pid: {}, base: 0x{:X})", pid, cradle::memory::baseAddress);
        announce_walkspeed_check();
    LogLightingSnapshot();
        cradle::overlay::Overlay::set_loading_stage(cradle::overlay::Overlay::LoadingStage::Initializing,
                                                    "Finalizing modules");

        cradle::overlay::Overlay::set_runtime_ready(true);
        cradle::overlay::Overlay::set_loading_stage(cradle::overlay::Overlay::LoadingStage::Ready,
                                                    "Overlay active");
        spdlog::info("overlay initialized - press delete to toggle menu");
    });

    while (overlay.isRunning())
    {
        overlay.render();
    }

    shutdown_requested.store(true, std::memory_order_relaxed);
    if (attach_thread.joinable())
        attach_thread.join();

    cradle::config::ConfigManager::tick(module_manager, true);
    overlay.cleanup();
    return 0;
}