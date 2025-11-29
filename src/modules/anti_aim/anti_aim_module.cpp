#include "anti_aim_module.hpp"

#include "overlay/overlay.hpp"
#include "util/memory/memory.hpp"

#include "cache/player_cache.hpp"
#include "util/engine/datamodel/datamodel.hpp"
#include "util/engine/offsets.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

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
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTwoPi = kPi * 2.0f;
    constexpr int kDefaultJitterSwapTicks = 5;
}

namespace cradle::modules
{
    AntiAimModule::AntiAimModule() : Module("anti aim", "forces physics sender bandwidth to desync locally")
    {
        keybind_mode = KeybindMode::TOGGLE;
        settings.push_back(Setting("spinbot enabled", false));
        settings.push_back(Setting("spinbot mode", 0, 0, 2));
        settings.push_back(Setting("spinbot speed", 0.3f, 0.05f, 2.0f));

        std::random_device rd;
        spin_rng_.seed(rd());
    }

    void AntiAimModule::on_render()
    {
        bool request_active = ShouldApplyDesync();
        TickNetworkDesync(request_active, kDefaultRestoreWindowMs);
        TickSpinbot();
    }

    void AntiAimModule::on_disable()
    {
        ResetHotkeyTracking();
        ResetSpinbotRandomOffset();
        RestoreAutoRotateOverride();
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

    void AntiAimModule::TickSpinbot()
    {
        if (!is_enabled())
        {
            RestoreAutoRotateOverride();
            ResetSpinbotRandomOffset();
            return;
        }

        auto enabled_setting = get_setting("spinbot enabled");
        if (!enabled_setting || !enabled_setting->value.bool_val)
        {
            RestoreAutoRotateOverride();
            ResetSpinbotRandomOffset();
            return;
        }

        auto mode_setting = get_setting("spinbot mode");
        int mode = 0;
        if (mode_setting)
        {
            mode = std::clamp(mode_setting->value.int_val, mode_setting->range.int_range.min, mode_setting->range.int_range.max);
        }

        auto local = cradle::engine::PlayerCache::get_local_player();
        if (!local.is_valid())
        {
            RestoreAutoRotateOverride();
            ResetSpinbotRandomOffset();
            return;
        }

        cradle::engine::Instance root_part = local.hrp.is_valid() ? local.hrp : local.torso;
        if (!root_part.is_valid())
        {
            RestoreAutoRotateOverride();
            ResetSpinbotRandomOffset();
            return;
        }

        uintptr_t root_primitive = ResolvePrimitive(root_part);
        if (!root_primitive)
        {
            RestoreAutoRotateOverride();
            ResetSpinbotRandomOffset();
            return;
        }

        if (mode == 2)
        {
            UpdateAutoRotateOverride(local, false);
            auto now = std::chrono::steady_clock::now();
            if (spin_random_pending_restore_)
            {
                if (root_primitive != spin_random_last_primitive_ || now >= spin_random_restore_deadline_)
                {
                    if (spin_random_last_primitive_ != 0 && cradle::memory::IsValid(spin_random_last_primitive_))
                    {
                        cradle::memory::write(spin_random_last_primitive_ + Offsets::BasePart::Position, spin_random_original_pos_);
                    }
                    spin_random_pending_restore_ = false;
                    spin_random_last_primitive_ = 0;
                }
                return;
            }

            spin_random_last_primitive_ = root_primitive;
            spin_random_original_pos_ = cradle::memory::read<cradle::engine::vector3>(root_primitive + Offsets::BasePart::Position);
            cradle::engine::vector3 offset(
                spin_random_offset_dist_(spin_rng_),
                spin_random_offset_dist_(spin_rng_),
                spin_random_offset_dist_(spin_rng_));
            cradle::engine::vector3 new_pos = spin_random_original_pos_ + offset;
            cradle::memory::write(root_primitive + Offsets::BasePart::Position, new_pos);
            spin_random_restore_deadline_ = now + std::chrono::milliseconds(1);
            spin_random_pending_restore_ = true;
            return;
        }

        ResetSpinbotRandomOffset();

        std::array<uintptr_t, 4> rotation_targets{};
        int rotation_count = 0;
        auto add_target = [&](uintptr_t candidate) {
            if (!candidate)
                return;
            for (int i = 0; i < rotation_count; ++i)
            {
                if (rotation_targets[i] == candidate)
                    return;
            }
            rotation_targets[rotation_count++] = candidate;
        };

        add_target(root_primitive);
        add_target(ResolvePrimitive(local.head));
        if (local.upper_torso.is_valid())
            add_target(ResolvePrimitive(local.upper_torso));
        else if (local.torso.is_valid())
            add_target(ResolvePrimitive(local.torso));
        if (rotation_count == 0)
        {
            UpdateAutoRotateOverride(local, false);
            return;
        }

        bool should_disable_auto_rotate = IsFirstPersonView(local);
        UpdateAutoRotateOverride(local, should_disable_auto_rotate);

        switch (mode)
        {
        case 1:
        {
            spin_jitter_counter_++;
            if (spin_jitter_counter_ > kDefaultJitterSwapTicks)
            {
                spin_jitter_direction_ = -spin_jitter_direction_;
                spin_jitter_counter_ = 0;
            }
            float angle = static_cast<float>(spin_jitter_direction_) * (kPi / 2.0f);
            for (int i = 0; i < rotation_count; ++i)
                ApplyYawRotation(rotation_targets[i], angle);
            break;
        }
        case 0:
        default:
        {
            auto speed_setting = get_setting("spinbot speed");
            float speed = speed_setting ? std::clamp(speed_setting->value.float_val,
                                                     speed_setting->range.float_range.min,
                                                     speed_setting->range.float_range.max)
                                        : 0.3f;
            spin_angle_ += speed;
            if (spin_angle_ > kTwoPi)
                spin_angle_ -= kTwoPi;
            for (int i = 0; i < rotation_count; ++i)
                ApplyYawRotation(rotation_targets[i], spin_angle_);
            break;
        }
        }
    }

    void AntiAimModule::ResetSpinbotRandomOffset()
    {
        if (!spin_random_pending_restore_)
            return;

        if (spin_random_last_primitive_ != 0 && cradle::memory::IsValid(spin_random_last_primitive_))
        {
            cradle::memory::write(spin_random_last_primitive_ + Offsets::BasePart::Position, spin_random_original_pos_);
        }

        spin_random_pending_restore_ = false;
        spin_random_last_primitive_ = 0;
    }

    void AntiAimModule::ApplyYawRotation(uintptr_t primitive, float angle_radians) const
    {
        cradle::engine::matrix3 spin_matrix;
        float cos_angle = std::cos(angle_radians);
        float sin_angle = std::sin(angle_radians);

        spin_matrix.data[0] = cos_angle;
        spin_matrix.data[1] = 0.0f;
        spin_matrix.data[2] = -sin_angle;
        spin_matrix.data[3] = 0.0f;
        spin_matrix.data[4] = 1.0f;
        spin_matrix.data[5] = 0.0f;
        spin_matrix.data[6] = sin_angle;
        spin_matrix.data[7] = 0.0f;
        spin_matrix.data[8] = cos_angle;

        cradle::memory::write(primitive + Offsets::BasePart::Rotation, spin_matrix);
    }

    uintptr_t AntiAimModule::ResolvePrimitive(const cradle::engine::Instance &part) const
    {
        if (!part.is_valid())
            return 0;

        uintptr_t primitive = cradle::memory::read<uintptr_t>(part.address + Offsets::BasePart::Primitive);
        if (primitive == 0 || !cradle::memory::IsValid(primitive))
            return 0;

        return primitive;
    }

    bool AntiAimModule::IsFirstPersonView(const cradle::engine::Player &local) const
    {
        if (!local.head.is_valid())
            return false;

        auto dm = cradle::engine::DataModel::get_instance();
        if (!dm.is_valid())
            return false;

        cradle::engine::Instance camera = dm.get_current_camera();
        if (!camera.is_valid())
            return false;

        cradle::engine::vector3 camera_pos = camera.get_pos();
        cradle::engine::vector3 head_pos = local.head.get_pos();

        constexpr float kFirstPersonThreshold = 1.5f;
        return camera_pos.distance(head_pos) < kFirstPersonThreshold;
    }

    void AntiAimModule::UpdateAutoRotateOverride(const cradle::engine::Player &local, bool disable_auto_rotate)
    {
        if (!disable_auto_rotate || !local.humanoid.is_valid())
        {
            RestoreAutoRotateOverride();
            return;
        }

        uintptr_t humanoid_addr = local.humanoid.address;
        if (!cradle::memory::IsValid(humanoid_addr))
        {
            RestoreAutoRotateOverride();
            return;
        }

        if (auto_rotate_override_humanoid_ != humanoid_addr)
        {
            RestoreAutoRotateOverride();
            auto_rotate_override_humanoid_ = humanoid_addr;
            auto_rotate_original_state_ = cradle::memory::read<std::uint8_t>(humanoid_addr + Offsets::Humanoid::AutoRotate);
        }

        cradle::memory::write<std::uint8_t>(humanoid_addr + Offsets::Humanoid::AutoRotate, 0);
        auto_rotate_override_active_ = true;
    }

    void AntiAimModule::RestoreAutoRotateOverride()
    {
        if (!auto_rotate_override_active_)
            return;

        if (auto_rotate_override_humanoid_ != 0 &&
            cradle::memory::IsValid(auto_rotate_override_humanoid_ + Offsets::Humanoid::AutoRotate))
        {
            cradle::memory::write<std::uint8_t>(auto_rotate_override_humanoid_ + Offsets::Humanoid::AutoRotate,
                                                auto_rotate_original_state_);
        }

        auto_rotate_override_active_ = false;
        auto_rotate_override_humanoid_ = 0;
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
                                                                                                                        
