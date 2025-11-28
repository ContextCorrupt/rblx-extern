#pragma once

#include "../module.hpp"
#include "../../cache/player.hpp"

#include <chrono>
#include <cstdint>

namespace cradle::modules
{
    class SilentAimModule : public Module
    {
    public:
        SilentAimModule();
        void on_update() override;

    private:
    bool acquire_target(engine::Player &out_player, engine::vector3 &out_world_pos, engine::vector2 &out_screen_pos);

        std::uint64_t last_target_address_{0};
        std::chrono::steady_clock::time_point last_target_time_{};
    };
}
