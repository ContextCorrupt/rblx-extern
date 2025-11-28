#include "anti_aim_module.hpp"

#include "cache/player_cache.hpp"
#include "overlay/overlay.hpp"

#include <algorithm>
#include <chrono>
#include <random>

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

using namespace cradle::engine;

namespace cradle::modules
{
    AntiAimModule::AntiAimModule() : Module("anti aim", "jitters your character to desync prediction")
    {
        keybind_mode = KeybindMode::TOGGLE;
        settings.push_back(Setting("underground mode", false));
        settings.push_back(Setting("horizontal jitter", 3.0f, 0.0f, 25.0f));
        settings.push_back(Setting("vertical jitter", 1.0f, 0.0f, 20.0f));
        settings.push_back(Setting("underground depth", 45.0f, 5.0f, 150.0f));
        settings.push_back(Setting("update interval (ms)", 5.0f, 0.0f, 100.0f));
    }

    void AntiAimModule::on_update()
    {
        if (overlay::Overlay::is_menu_open())
            return;

        auto interval_setting = get_setting("update interval (ms)");
        float interval_ms = interval_setting ? interval_setting->value.float_val : 5.0f;
        interval_ms = std::clamp(interval_ms, 0.0f, 250.0f);
        auto interval = std::chrono::milliseconds(static_cast<int>(interval_ms));

        auto now = std::chrono::steady_clock::now();
        if (last_update_.time_since_epoch().count() != 0 && now - last_update_ < interval)
            return;
        last_update_ = now;

        Player local = PlayerCache::get_local_player();
        if (!local.is_valid() || !local.hrp.is_valid())
            return;

        auto horizontal_setting = get_setting("horizontal jitter");
        auto vertical_setting = get_setting("vertical jitter");
        auto underground_setting = get_setting("underground mode");
        auto depth_setting = get_setting("underground depth");

        float horizontal = horizontal_setting ? horizontal_setting->value.float_val : 0.0f;
        float vertical = vertical_setting ? vertical_setting->value.float_val : 0.0f;
        bool underground = underground_setting && underground_setting->value.bool_val;
        float depth = depth_setting ? depth_setting->value.float_val : 0.0f;

        vector3 base_pos = local.hrp.get_pos();
        if (base_pos.X == 0.0f && base_pos.Y == 0.0f && base_pos.Z == 0.0f)
            return;

        jitter_flip_ = !jitter_flip_;
        float horizontal_offset = jitter_flip_ ? horizontal : -horizontal;

        static thread_local std::mt19937 rng(std::random_device{}());
        std::uniform_real_distribution<float> vertical_dist(vertical > 0.0f ? -vertical : 0.0f, vertical > 0.0f ? vertical : 0.0f);
        float vertical_random = vertical_dist(rng);

        vector3 new_pos = base_pos;
        new_pos.X += horizontal_offset;
        new_pos.Z -= horizontal_offset;

        if (underground)
            new_pos.Y = base_pos.Y - std::max(depth, 0.0f) + vertical_random;
        else
            new_pos.Y = base_pos.Y + vertical_random;

        local.hrp.set_pos(new_pos);
    }
}
