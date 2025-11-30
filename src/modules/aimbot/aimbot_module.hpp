#pragma once

#include "../module.hpp"
#include "../../cache/player.hpp"
#include <cstdint>

namespace cradle::modules
{
    class AimbotModule : public Module
    {
    public:
        AimbotModule();
        void on_update() override;
        void on_render() override;
        bool allow_render_when_disabled() override;

    private:
        uintptr_t locked_character_address = 0;
    };
}
