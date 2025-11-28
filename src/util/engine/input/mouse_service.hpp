#pragma once

#include "../math.hpp"

namespace cradle::engine
{
    class MouseService
    {
    public:
        static bool write_screen_position(const vector2 &screen_position);
        static bool write_screen_position(float x, float y)
        {
            return write_screen_position(vector2(x, y));
        }
        static bool is_available();
    };
}
