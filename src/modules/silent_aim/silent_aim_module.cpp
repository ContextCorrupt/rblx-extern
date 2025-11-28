#include "silent_aim_module.hpp"

#include "cache/player_cache.hpp"
#include "modules/friend_manager.hpp"
#include "overlay/overlay.hpp"
#include "util/engine/datamodel/datamodel.hpp"
#include "util/engine/input/mouse_service.hpp"
#include "util/engine/visualengine/visualengine.hpp"
#include "util/engine/wallcheck/wallcheck.hpp"
#include "util/renderer.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

using namespace cradle::engine;

namespace cradle::modules
{
    namespace
    {
        constexpr std::chrono::milliseconds kStickyGrace(500);
    }

    SilentAimModule::SilentAimModule() : Module("silent aim", "spoofs aim direction without moving your camera")
    {
        keybind_mode = KeybindMode::HOLD;
        settings.push_back(Setting("fov size", 75.0f, 5.0f, 500.0f));
        settings.push_back(Setting("wall check", true));
        settings.push_back(Setting("team check", true));
        settings.push_back(Setting("stick to last target", true));
        settings.push_back(Setting("max distance", 2000.0f, 0.0f, 10000.0f));
        settings.push_back(Setting("fallback cursor move", false));
    }

    void SilentAimModule::on_update()
    {
        if (overlay::Overlay::is_menu_open())
            return;

        Player target;
        vector3 target_world;
        vector2 target_screen;
        if (!acquire_target(target, target_world, target_screen))
            return;

        if (MouseService::write_screen_position(target_screen))
            return;

        auto fallback_setting = get_setting("fallback cursor move");
        if (!fallback_setting || !fallback_setting->value.bool_val)
            return;

        POINT overlay_origin = overlay::Overlay::get_overlay_origin();
        LONG screen_x = overlay_origin.x + static_cast<LONG>(std::round(target_screen.X));
        LONG screen_y = overlay_origin.y + static_cast<LONG>(std::round(target_screen.Y));
        SetCursorPos(screen_x, screen_y);
    }

    bool SilentAimModule::acquire_target(Player &out_player, vector3 &out_world_pos, vector2 &out_screen_pos)
    {
        auto fov_setting = get_setting("fov size");
        auto wall_check_setting = get_setting("wall check");
        auto team_check_setting = get_setting("team check");
        auto sticky_setting = get_setting("stick to last target");
        auto max_distance_setting = get_setting("max distance");

        if (!fov_setting || !wall_check_setting || !team_check_setting || !sticky_setting)
            return false;

        float fov = fov_setting->value.float_val;
        bool wall_check = wall_check_setting->value.bool_val;
        bool team_check = team_check_setting->value.bool_val;
        bool stick_to_last = sticky_setting->value.bool_val;
        float max_distance = max_distance_setting ? max_distance_setting->value.float_val : 0.0f;

        auto players_snapshot = PlayerCache::get_players();
        if (!players_snapshot || players_snapshot->empty())
            return false;
        const auto &players = *players_snapshot;

        Player local = PlayerCache::get_local_player();
        if (!local.is_valid() || !local.hrp.is_valid())
            return false;

        DataModel dm = DataModel::get_instance();
        if (!dm.is_valid())
            return false;

        Instance camera = dm.get_current_camera();
        if (!camera.is_valid())
            return false;

        VisualEngine ve = VisualEngine::get_instance();
        if (!ve.is_valid())
            return false;

        matrix4 view_matrix = ve.get_viewmatrix();
        vector2 screen_size = ve.get_dimensions();

        HWND roblox_window = FindWindowA(nullptr, "Roblox");
        POINT cursor_point{static_cast<LONG>(screen_size.X / 2.0f), static_cast<LONG>(screen_size.Y / 2.0f)};
        if (roblox_window)
        {
            if (!GetCursorPos(&cursor_point) || !ScreenToClient(roblox_window, &cursor_point))
            {
                cursor_point.x = static_cast<LONG>(screen_size.X / 2.0f);
                cursor_point.y = static_cast<LONG>(screen_size.Y / 2.0f);
            }
        }

        vector2 cursor(static_cast<float>(cursor_point.x), static_cast<float>(cursor_point.y));
        vector3 camera_pos = camera.get_pos();

        const float fov_sq = fov * fov;
        const float max_distance_sq = max_distance > 0.0f ? max_distance * max_distance : std::numeric_limits<float>::max();

        struct Candidate
        {
            Player player;
            vector3 world_pos;
            vector2 screen_pos;
            float metric = std::numeric_limits<float>::max();
        };

        auto evaluate_candidate = [&](const Player &candidate, Candidate &result) -> bool {
            if (!candidate.is_valid() || !candidate.head.is_valid() || !candidate.character.is_valid())
                return false;
            if (candidate.character.address == local.character.address)
                return false;
            if (candidate.health <= 0.0f)
                return false;
            if (team_check && !candidate.team.empty() && !local.team.empty() && candidate.team == local.team)
                return false;
            if (friends::is_friend(candidate.name))
                return false;

            vector3 head_pos = candidate.head.get_pos();
            if (head_pos.X == 0.0f && head_pos.Y == 0.0f && head_pos.Z == 0.0f)
                return false;

            vector2 screen = cradle::world_to_screen(head_pos, view_matrix, screen_size);
            if (screen.X == -1.0f || screen.Y == -1.0f)
                return false;

            float dx = screen.X - cursor.X;
            float dy = screen.Y - cursor.Y;
            float screen_dist_sq = dx * dx + dy * dy;
            if (screen_dist_sq > fov_sq)
                return false;

            vector3 diff = head_pos - camera_pos;
            float world_dist_sq = diff.X * diff.X + diff.Y * diff.Y + diff.Z * diff.Z;
            if (world_dist_sq > max_distance_sq)
                return false;

            if (wall_check && Wallcheck::is_cache_ready())
            {
                if (!Wallcheck::is_visible(camera_pos, head_pos, head_pos, head_pos, candidate.character.address))
                    return false;
            }

            result.player = candidate;
            result.world_pos = head_pos;
            result.screen_pos = screen;
            result.metric = screen_dist_sq;
            return true;
        };

        Candidate best_candidate;
        bool found_candidate = false;

        if (stick_to_last && last_target_address_ != 0)
        {
            auto sticky = std::find_if(players.begin(), players.end(), [&](const Player &p) {
                return p.character.address == last_target_address_;
            });

            if (sticky != players.end())
            {
                Candidate cached_candidate;
                if (evaluate_candidate(*sticky, cached_candidate))
                {
                    auto now = std::chrono::steady_clock::now();
                    if (last_target_time_.time_since_epoch().count() == 0 || now - last_target_time_ <= kStickyGrace)
                    {
                        out_player = cached_candidate.player;
                        out_world_pos = cached_candidate.world_pos;
                        out_screen_pos = cached_candidate.screen_pos;
                        last_target_time_ = now;
                        return true;
                    }
                }
            }
        }

    for (const auto &player : players)
        {
            Candidate candidate;
            if (!evaluate_candidate(player, candidate))
                continue;

            if (!found_candidate || candidate.metric < best_candidate.metric)
            {
                best_candidate = candidate;
                found_candidate = true;
            }
        }

        if (!found_candidate)
        {
            last_target_address_ = 0;
            return false;
        }

        out_player = best_candidate.player;
        out_world_pos = best_candidate.world_pos;
        out_screen_pos = best_candidate.screen_pos;
        last_target_address_ = best_candidate.player.character.address;
        last_target_time_ = std::chrono::steady_clock::now();
        return true;
    }

}
