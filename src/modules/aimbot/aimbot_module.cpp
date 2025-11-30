#include "aimbot_module.hpp"
#include "cache/player_cache.hpp"
#include "util/engine/visualengine/visualengine.hpp"
#include "util/engine/datamodel/datamodel.hpp"
#include "util/engine/wallcheck/wallcheck.hpp"
#include "util/renderer.hpp"
#include "overlay/overlay.hpp"
#include "modules/friend_manager.hpp"
#include <imgui.h>
#include <Windows.h>
#include <cmath>

using namespace cradle::engine;

namespace cradle::modules
{

    AimbotModule::AimbotModule() : Module("aimbot", "locks onto closest player")
    {
        keybind_mode = KeybindMode::HOLD;

        settings.push_back(Setting("fov size", 100.0f, 10.0f, 500.0f));
        settings.push_back(Setting("fov circle", true));
        settings.push_back(Setting("fov always visible", false));
        settings.push_back(Setting("wall check", true));
        settings.push_back(Setting("team check", true));
        settings.push_back(Setting("target lock", false));
        settings.push_back(Setting("smoothness", 0.0f, 0.0f, 0.9995f));
        settings.push_back(Setting("fov color", 1.0f, 1.0f, 1.0f, 1.0f));
        settings.push_back(Setting("target priority mode", 0, 0, 2));
        settings.push_back(Setting("max distance", 2000.0f, 0.0f, 10000.0f));
    }

    void AimbotModule::on_update()
    {
            if (overlay::Overlay::is_menu_open())
                return;
            auto fov_size_setting = get_setting("fov size");
            auto wall_check_setting = get_setting("wall check");
            auto team_check_setting = get_setting("team check");
            auto target_lock_setting = get_setting("target lock");
            auto smoothness_setting = get_setting("smoothness");
            auto target_priority_setting = get_setting("target priority mode");

            if (!fov_size_setting || !wall_check_setting || !team_check_setting || !target_lock_setting || !smoothness_setting || !target_priority_setting)
                return;

            auto clear_locked_target = [&]() { locked_character_address = 0; };

            float fov_size = fov_size_setting->value.float_val;
            bool wall_check = wall_check_setting->value.bool_val;
            bool team_check = team_check_setting->value.bool_val;
            bool target_lock_enabled = target_lock_setting->value.bool_val;
            float smoothness = smoothness_setting->value.float_val;
            int target_priority_mode = target_priority_setting->value.int_val;
            const float fov_size_sq = fov_size * fov_size;
            if (target_priority_mode < 0 || target_priority_mode > 2)
            {
                target_priority_mode = 0;
                target_priority_setting->value.int_val = 0;
            }

            auto players_snapshot = PlayerCache::get_players();
            if (!players_snapshot || players_snapshot->empty())
                return;
            const auto &players = *players_snapshot;

            auto max_distance_setting = get_setting("max distance");
            float max_distance = 2000.0f;
            float max_distance_sq = FLT_MAX;
            if (max_distance_setting)
            {
                max_distance = max_distance_setting->value.float_val;
                max_distance_sq = max_distance * max_distance;
            }

            auto local = PlayerCache::get_local_player();
            if (!local.is_valid() || !local.hrp.is_valid())
                return;

            std::string local_team = local.team;

            using cradle::engine::VisualEngine;
            VisualEngine ve = VisualEngine::get_instance();
            if (!ve.is_valid())
                return;

            matrix4 vm = ve.get_viewmatrix();
            vector2 screen_size((float)GetSystemMetrics(SM_CXSCREEN), (float)GetSystemMetrics(SM_CYSCREEN));

            POINT cursor_pos;
            GetCursorPos(&cursor_pos);

            HWND roblox_window = FindWindowA(nullptr, "Roblox");
            POINT cursor_client = cursor_pos;
            if (roblox_window)
            {
                ScreenToClient(roblox_window, &cursor_client);
            }

            DataModel dm = DataModel::get_instance();
            if (!dm.is_valid())
                return;

            Instance camera = dm.get_current_camera();
            if (!camera.is_valid())
                return;

            vector3 camera_pos = camera.get_pos();

            if (wall_check)
            {
                if (!Wallcheck::is_cache_ready())
                    return;
            }

            bool found_target = false;
            Player closest_player;
            float best_metric = FLT_MAX;
            float best_secondary_metric = FLT_MAX;
            Player locked_candidate;
            bool locked_candidate_valid = false;

            for (const auto &p : players)
            {
                if (!p.is_valid() || !p.head.is_valid())
                    continue;
                if (!p.character.is_valid() || !p.hrp.is_valid())
                    continue;

                bool is_locked_target = target_lock_enabled && locked_character_address != 0 && p.character.address == locked_character_address;

                if (p.character.address == local.character.address)
                    continue;
                if (p.hrp.address == local.hrp.address)
                    continue;
                if (p.health <= 0.0f)
                    continue;

                bool player_visible = true;
                if (p.humanoid.is_valid())
                {
                    player_visible = p.humanoid.get_visible_flag();
                }
                if (!player_visible)
                    continue;

                if (team_check && !p.team.empty() && !local_team.empty() && p.team == local_team)
                    continue;

                if (friends::is_friend(p.name))
                    continue;

                vector3 target_pos = p.head.is_valid() ? p.head.get_cframe().position : vector3();
                if (target_pos.X == 0 && target_pos.Y == 0 && target_pos.Z == 0)
                    continue;

                // filter by max world distance (apply regardless of target_by_distance mode)
                vector3 world_diff = target_pos - camera_pos;
                float world_dist_sq = world_diff.X * world_diff.X + world_diff.Y * world_diff.Y + world_diff.Z * world_diff.Z;
                if (max_distance_setting && world_dist_sq > max_distance_sq)
                    continue;

                auto screen_pos = world_to_screen(target_pos, vm, screen_size);
                // world_to_screen returns -1 for invalid/offscreen points; check explicitly
                if (screen_pos.X == -1 || screen_pos.Y == -1)
                    continue;

                // Compute squared 2D screen distance and first filter by FOV circle (avoid sqrt)
                float dx_screen = screen_pos.X - cursor_pos.x;
                float dy_screen = screen_pos.Y - cursor_pos.y;
                float screen_dist_sq = dx_screen * dx_screen + dy_screen * dy_screen;
                if (screen_dist_sq > fov_size_sq)
                    continue;

                if (wall_check && Wallcheck::is_cache_ready())
                {
                    auto fetch_pos = [](const Instance &inst, const vector3 &fallback) {
                        return inst.is_valid() ? inst.get_pos() : fallback;
                    };
                    vector3 head_pos = p.head.get_pos();
                    vector3 torso_pos = fetch_pos(p.hrp.is_valid() ? p.hrp : p.upper_torso, head_pos);
                    vector3 pelvis_pos = fetch_pos(p.lower_torso, torso_pos);
                    if (!Wallcheck::is_visible(camera_pos, head_pos, torso_pos, pelvis_pos, p.character.address))
                        continue;
                }

                if (is_locked_target)
                {
                    locked_candidate = p;
                    locked_candidate_valid = true;
                }

                float priority_metric = screen_dist_sq;
                switch (target_priority_mode)
                {
                case 1:
                    priority_metric = world_dist_sq;
                    break;
                case 2:
                    priority_metric = p.health;
                    break;
                default:
                    break;
                }

                float secondary_metric = screen_dist_sq;

                if (priority_metric < best_metric ||
                    (std::fabs(priority_metric - best_metric) < 1e-3f && secondary_metric < best_secondary_metric))
                {
                    best_metric = priority_metric;
                    best_secondary_metric = secondary_metric;
                    closest_player = p;
                    found_target = true;
                }
            }

            if (target_lock_enabled && locked_candidate_valid)
            {
                closest_player = locked_candidate;
                found_target = true;
            }

            if (!found_target)
            {
                clear_locked_target();
                return;
            }
            if (!closest_player.is_valid() || !closest_player.character.is_valid())
            {
                clear_locked_target();
                return;
            }
            if (!closest_player.head.is_valid())
            {
                clear_locked_target();
                return;
            }
            if (closest_player.humanoid.is_valid())
            {
                if (!closest_player.humanoid.get_visible_flag())
                {
                    clear_locked_target();
                    return;
                }
            }

            vector3 target_pos = closest_player.head.is_valid() ? closest_player.head.get_cframe().position : vector3();
            if (target_pos.X == 0 && target_pos.Y == 0 && target_pos.Z == 0)
            {
                clear_locked_target();
                return;
            }

            if (wall_check && Wallcheck::is_cache_ready())
            {
                auto fetch_pos = [](const Instance &inst, const vector3 &fallback) {
                    return inst.is_valid() ? inst.get_pos() : fallback;
                };
                vector3 head_pos = closest_player.head.get_pos();
                vector3 torso_pos = fetch_pos(closest_player.hrp.is_valid() ? closest_player.hrp : closest_player.upper_torso, head_pos);
                vector3 pelvis_pos = fetch_pos(closest_player.lower_torso, torso_pos);
                if (!Wallcheck::is_visible(camera_pos, head_pos, torso_pos, pelvis_pos, closest_player.character.address))
                {
                    clear_locked_target();
                    return;
                }
            }

            if (target_lock_enabled)
                locked_character_address = closest_player.character.address;
            else
                clear_locked_target();

            // Determine whether the in-game cursor is locked by checking if the cursor
            // is approximately at the center of the Roblox client window. This is
            // more reliable than previous heuristics which compared different coordinate spaces.
            bool cursor_locked = false;
            if (roblox_window)
            {
                RECT rc;
                if (GetClientRect(roblox_window, &rc))
                {
                    POINT center_client = { (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };
                    POINT center_screen = center_client;
                    ClientToScreen(roblox_window, &center_screen);
                    int dxCenter = cursor_pos.x - center_screen.x;
                    int dyCenter = cursor_pos.y - center_screen.y;
                    // within 25 pixels of center => consider cursor locked
                    cursor_locked = (dxCenter * dxCenter + dyCenter * dyCenter) <= (25 * 25);
                }
            }

            if (cursor_locked)
            {
                vector3 direction = (target_pos - camera_pos).normalize();
                vector3 up(0, 1, 0);
                vector3 right = direction.cross(up).normalize();
                vector3 real_up = right.cross(direction).normalize();

                matrix3 target_rot;
                target_rot.data[0] = right.X;
                target_rot.data[1] = real_up.X;
                target_rot.data[2] = -direction.X;
                target_rot.data[3] = right.Y;
                target_rot.data[4] = real_up.Y;
                target_rot.data[5] = -direction.Y;
                target_rot.data[6] = right.Z;
                target_rot.data[7] = real_up.Z;
                target_rot.data[8] = -direction.Z;

                // clamp smoothness to [0,0.9995] to avoid negative interpolation and extreme jumps
                if (smoothness < 0.0f) smoothness = 0.0f;
                if (smoothness > 0.9995f) smoothness = 0.9995f;

                if (smoothness > 0.0f)
                {
                    matrix3 current_rot = cradle::memory::read<matrix3>(camera.address + Offsets::Camera::Rotation);

                    float t = 1.0f - smoothness;

                    matrix3 smoothed_rot;
                    for (int i = 0; i < 9; i++)
                    {
                        smoothed_rot.data[i] = current_rot.data[i] + (target_rot.data[i] - current_rot.data[i]) * t;
                    }

                    cradle::memory::write<matrix3>(camera.address + Offsets::Camera::Rotation, smoothed_rot);
                }
                else
                {
                    cradle::memory::write<matrix3>(camera.address + Offsets::Camera::Rotation, target_rot);
                }
            }
            else
            {
                auto target_screen = world_to_screen(target_pos, vm, screen_size);

                // ensure valid screen coordinates
                if (target_screen.X != -1 && target_screen.Y != -1)
                {
                    float dx = target_screen.X - cursor_pos.x;
                    float dy = target_screen.Y - cursor_pos.y;

                    dx *= (1.0f - smoothness);
                    dy *= (1.0f - smoothness);

                    if (!(std::abs(dx) < 0.5f && std::abs(dy) < 0.5f))
                    {
                        int new_x = cursor_pos.x + (int)dx;
                        int new_y = cursor_pos.y + (int)dy;
                        SetCursorPos(new_x, new_y);
                    }
                }
            }
            }

    void AimbotModule::on_render()
    {
        auto show_fov_setting = get_setting("fov circle");
        auto fov_always_setting = get_setting("fov always visible");
        if (!show_fov_setting || !show_fov_setting->value.bool_val)
            return;

        bool always_visible = fov_always_setting && fov_always_setting->value.bool_val;
        if (!is_enabled() && !always_visible)
            return;

        auto fov_size_setting = get_setting("fov size");
        auto fov_color_setting = get_setting("fov color");

        if (!fov_size_setting || !fov_color_setting)
            return;

        float fov_size = fov_size_setting->value.float_val;

        ImU32 fov_color = IM_COL32(
            (int)(fov_color_setting->value.color_val[0] * 255),
            (int)(fov_color_setting->value.color_val[1] * 255),
            (int)(fov_color_setting->value.color_val[2] * 255),
            (int)(fov_color_setting->value.color_val[3] * 255));

        ImGuiIO &io = ImGui::GetIO();
        ImVec2 fov_center = io.MousePos;

        bool using_imgui_mouse = overlay::Overlay::is_menu_open() || io.WantCaptureMouse;
        if (!using_imgui_mouse)
        {
            POINT cursor;
            if (GetCursorPos(&cursor))
            {
                POINT origin = overlay::Overlay::get_overlay_origin();
                fov_center.x = static_cast<float>(cursor.x - origin.x);
                fov_center.y = static_cast<float>(cursor.y - origin.y);
            }
            else
            {
                VisualEngine ve = VisualEngine::get_instance();
                if (ve.is_valid())
                {
                    vector2 dims = ve.get_dimensions();
                    fov_center.x = dims.X / 2.0f;
                    fov_center.y = dims.Y / 2.0f;
                }
            }
        }

        ImDrawList *draw_list = ImGui::GetBackgroundDrawList();
        if (draw_list)
        {
            draw_list->AddCircle(fov_center, fov_size, fov_color, 64, 2.0f);
        }
    }

    bool AimbotModule::allow_render_when_disabled()
    {
        auto show_fov_setting = get_setting("fov circle");
        auto fov_always_setting = get_setting("fov always visible");
        if (!show_fov_setting || !show_fov_setting->value.bool_val)
            return false;
        return fov_always_setting && fov_always_setting->value.bool_val;
    }
}
