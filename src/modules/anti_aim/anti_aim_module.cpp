#include "anti_aim_module.hpp"

#include "overlay/overlay.hpp"
#include "util/memory/memory.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace
{
    constexpr uintptr_t kPhysicsSenderMaxBandwidthOffset = 0x65e426c; // DFIntPhysicsSenderMaxBandwidthBps
    constexpr float kDefaultRestoreWindowMs = 3000.0f;
}

namespace cradle::modules
{
    AntiAimModule::AntiAimModule() : Module("anti aim", "forces physics sender bandwidth to desync locally")
    {
        keybind_mode = KeybindMode::TOGGLE;
    }

    void AntiAimModule::on_render()
    {
        bool request_active = ShouldApplyDesync();
        TickNetworkDesync(request_active, kDefaultRestoreWindowMs);
    }

    void AntiAimModule::on_disable()
    {
        ResetHotkeyTracking();
    }

    bool AntiAimModule::allow_render_when_disabled()
    {
        return true;
    }

    int AntiAimModule::get_desync_hotkey() const
    {
        return desync_hotkey_;
    }

    void AntiAimModule::set_desync_hotkey(int key)
    {
        if (desync_hotkey_ == key)
            return;
        desync_hotkey_ = key;
        ResetHotkeyTracking();
        cradle::config::mark_dirty();
    }

    KeybindMode AntiAimModule::get_desync_hotkey_mode() const
    {
        return desync_hotkey_mode_;
    }

    void AntiAimModule::set_desync_hotkey_mode(KeybindMode mode)
    {
        if (desync_hotkey_mode_ == mode)
            return;
        desync_hotkey_mode_ = mode;
        ResetHotkeyTracking();
        cradle::config::mark_dirty();
    }

    bool AntiAimModule::ShouldApplyDesync() const
    {
        if (!is_enabled())
        {
            ResetHotkeyTracking();
            return false;
        }

        if (desync_hotkey_ == 0)
        {
            desync_hotkey_toggle_state_ = true;
            return true;
        }

        return PollHotkeyState();
    }

    bool AntiAimModule::PollHotkeyState() const
    {
        bool key_down = false;
        if (!overlay::Overlay::is_menu_open())
        {
            key_down = (GetAsyncKeyState(desync_hotkey_) & 0x8000) != 0;
        }

        if (desync_hotkey_mode_ == KeybindMode::HOLD)
        {
            desync_hotkey_was_down_ = key_down;
            desync_hotkey_toggle_state_ = key_down;
            return key_down;
        }

        if (key_down && !desync_hotkey_was_down_)
        {
            desync_hotkey_toggle_state_ = !desync_hotkey_toggle_state_;
        }

        desync_hotkey_was_down_ = key_down;
        return desync_hotkey_toggle_state_;
    }

    void AntiAimModule::ResetHotkeyTracking() const
    {
        desync_hotkey_was_down_ = false;
        desync_hotkey_toggle_state_ = false;
    }

    void AntiAimModule::TickNetworkDesync(bool request_active, float restore_window_ms)
    {
        restore_window_ms = std::clamp(restore_window_ms, 0.0f, 10000.0f);

        auto reset_state = [&]() {
            desync_lock_active_ = false;
            desync_restoring_ = false;
            desync_has_original_ = false;
        };

        if (cradle::memory::baseAddress == 0)
        {
            reset_state();
            return;
        }

        uintptr_t bandwidth_addr = cradle::memory::baseAddress + kPhysicsSenderMaxBandwidthOffset;
        if (!cradle::memory::IsValid(bandwidth_addr))
        {
            reset_state();
            return;
        }

        auto write_value = [&](std::int32_t value) {
            cradle::memory::write<std::int32_t>(bandwidth_addr, value);
        };

        if (request_active)
        {
            if (!desync_has_original_)
            {
                desync_original_value_ = cradle::memory::read<std::int32_t>(bandwidth_addr);
                desync_has_original_ = true;
            }

            write_value(0);
            desync_lock_active_ = true;
            desync_restoring_ = false;
            return;
        }

        if (desync_lock_active_)
        {
            desync_lock_active_ = false;
            if (desync_has_original_)
            {
                desync_restoring_ = true;
                desync_restore_start_ = std::chrono::steady_clock::now();
            }
        }

        if (desync_restoring_ && desync_has_original_)
        {
            auto now = std::chrono::steady_clock::now();
            auto restore_duration = std::chrono::milliseconds(static_cast<int>(restore_window_ms));

            write_value(desync_original_value_);

            if (restore_window_ms <= 0.0f || now - desync_restore_start_ >= restore_duration)
            {
                desync_restoring_ = false;
                desync_has_original_ = false;
                desync_original_value_ = 0;
            }
        }
        else if (desync_has_original_)
        {
            // ensure the original value is restored if we exit early without a timed window
            write_value(desync_original_value_);
            desync_has_original_ = false;
            desync_original_value_ = 0;
        }
    }
}
                                                                                                                        
