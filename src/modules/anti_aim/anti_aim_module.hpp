#pragma once

#include "../module.hpp"

#include "../../util/engine/math.hpp"

#include <chrono>
#include <cstdint>
#include <random>

namespace cradle::engine
{
    class Instance;
    struct Player;
}

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

        void TickSpinbot();
        void ResetSpinbotRandomOffset();
        void ApplyYawRotation(uintptr_t primitive, float angle_radians) const;
    uintptr_t ResolvePrimitive(const cradle::engine::Instance &part) const;
    bool IsFirstPersonView(const cradle::engine::Player &local) const;
    void UpdateAutoRotateOverride(const cradle::engine::Player &local, bool disable_auto_rotate);
    void RestoreAutoRotateOverride();

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

        float spin_angle_ = 0.0f;
        int spin_jitter_direction_ = 1;
        int spin_jitter_counter_ = 0;
        std::mt19937 spin_rng_{};
        std::uniform_real_distribution<float> spin_random_offset_dist_{-5.0f, 5.0f};
        bool spin_random_pending_restore_ = false;
        uintptr_t spin_random_last_primitive_ = 0;
        cradle::engine::vector3 spin_random_original_pos_{};
        std::chrono::steady_clock::time_point spin_random_restore_deadline_{};

        bool auto_rotate_override_active_ = false;
        uintptr_t auto_rotate_override_humanoid_ = 0;
        std::uint8_t auto_rotate_original_state_ = 1;
    };
}
