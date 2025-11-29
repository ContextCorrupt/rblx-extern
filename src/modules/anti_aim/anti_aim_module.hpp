#pragma once

#include "../module.hpp"

#include <chrono>
#include <cstdint>

namespace cradle::modules
{
    class AntiAimModule : public Module
    {
    public:
        AntiAimModule();
        void on_render() override;
        void on_disable() override;
        bool allow_render_when_disabled() override;

        int get_desync_hotkey() const;
        void set_desync_hotkey(int key);
        KeybindMode get_desync_hotkey_mode() const;
        void set_desync_hotkey_mode(KeybindMode mode);

    private:
        bool ShouldApplyDesync() const;
        bool PollHotkeyState() const;
        void ResetHotkeyTracking() const;

        void TickNetworkDesync(bool request_active, float restore_window_ms);

        bool desync_lock_active_ = false;
        bool desync_restoring_ = false;
        bool desync_has_original_ = false;
        std::int32_t desync_original_value_ = 0;
        std::chrono::steady_clock::time_point desync_restore_start_{};

        int desync_hotkey_ = 0;
        KeybindMode desync_hotkey_mode_ = KeybindMode::TOGGLE;
        mutable bool desync_hotkey_was_down_ = false;
        mutable bool desync_hotkey_toggle_state_ = false;
    };
}
