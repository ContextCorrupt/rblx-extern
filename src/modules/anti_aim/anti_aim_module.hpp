#pragma once

#include "../module.hpp"
#include "../../cache/player.hpp"

#include <chrono>

namespace cradle::modules
{
    class AntiAimModule : public Module
    {
    public:
        AntiAimModule();
        void on_update() override;

    private:
        std::chrono::steady_clock::time_point last_update_{};
        bool jitter_flip_ = false;
    };
}
