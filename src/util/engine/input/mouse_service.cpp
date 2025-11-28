#include "mouse_service.hpp"

#include "../datamodel/datamodel.hpp"
#include "../instance/instance.hpp"
#include "../offsets.hpp"
#include "../../memory/memory.hpp"

#include <chrono>
#include <cmath>
#include <mutex>

namespace cradle::engine
{
    namespace
    {
        std::mutex g_mouse_mutex;
        std::uint64_t g_cached_service = 0;
        std::uint64_t g_cached_input_object = 0;
        std::chrono::steady_clock::time_point g_last_refresh;
        constexpr auto kCacheLifetime = std::chrono::milliseconds(500);

        bool refresh_locked()
        {
            DataModel dm = DataModel::get_instance();
            if (!dm.is_valid())
                return false;

            Instance mouse_service = dm.find_first_child("MouseService");
            if (!mouse_service.is_valid())
                return false;

            std::uint64_t input_object = cradle::memory::read<std::uint64_t>(mouse_service.address + Offsets::MouseService::InputObject);
            if (input_object <= 0x10000)
                return false;

            g_cached_service = mouse_service.address;
            g_cached_input_object = input_object;
            g_last_refresh = std::chrono::steady_clock::now();
            return true;
        }

        std::uint64_t resolve_input_object()
        {
            std::lock_guard<std::mutex> lock(g_mouse_mutex);
            auto now = std::chrono::steady_clock::now();
            if (g_cached_input_object > 0x10000 && (now - g_last_refresh) < kCacheLifetime)
            {
                return g_cached_input_object;
            }

            if (refresh_locked())
                return g_cached_input_object;

            return 0;
        }
    }

    bool MouseService::write_screen_position(const vector2 &screen_position)
    {
        if (!std::isfinite(screen_position.X) || !std::isfinite(screen_position.Y))
            return false;

        vector2 clamped = screen_position;
        if (clamped.X < 0.0f)
            clamped.X = 0.0f;
        if (clamped.Y < 0.0f)
            clamped.Y = 0.0f;

        std::uint64_t input_object = resolve_input_object();
        if (input_object <= 0x10000)
            return false;

        cradle::memory::write<vector2>(input_object + Offsets::MouseService::MousePosition, clamped);
        return true;
    }

    bool MouseService::is_available()
    {
        return resolve_input_object() > 0x10000;
    }
}
