#include "overlay.hpp"
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <windows.h>
#include <windowsx.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/fmt/ostr.h>
#include <fmt/format.h>
#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <dwmapi.h>
#include <limits>
#include <mutex>
#include <atomic>
#include "../util/engine/visualengine/visualengine.hpp"
#include "../util/engine/datamodel/datamodel.hpp"
#include "../util/engine/instance/instance.hpp"
#include "../util/engine/wallcheck/wallcheck.hpp"
#include "../util/engine/lighting/lighting.hpp"
#include "../util/engine/offsets.hpp"
#include "../cache/player_cache.hpp"
#include "../modules/module_manager.hpp"
#include "../modules/friend_manager.hpp"
#include "../modules/anti_aim/anti_aim_module.hpp"
#include "../config/config_manager.hpp"
#include "../util/memory/memory.hpp"
#include "../evo/inc.hpp"
#include "../util/profiling/profiler.hpp"

#pragma comment(lib, "dwmapi.lib")

#undef min
#undef max

namespace cradle
{
    namespace overlay
    {
        namespace
        {
            constexpr wchar_t kOverlayWindowClass[] = L"CradleOverlayWindow";

            enum class EvoTab : int
            {
                Aimbot = 0,
                Esp,
                Misc,
                Config,
                Count
            };

            struct TabInfo
            {
                EvoTab tab;
                const char *icon;
                const char *label;
            };

            enum class MemoryPanelSection
            {
                None = -1,
                Walkspeed = 0,
                JumpHeight,
                Gravity,
                Lighting
            };

            struct ModuleListEntry
            {
                std::string label;
                cradle::modules::Module *module;
                MemoryPanelSection memory_section = MemoryPanelSection::None;
            };

            const std::array<TabInfo, 4> kTabs = {
                TabInfo{EvoTab::Aimbot, "i", "aimbot"},
                TabInfo{EvoTab::Esp, "k", "esp"},
                TabInfo{EvoTab::Misc, "3", "misc"},
                TabInfo{EvoTab::Config, "4", "config"}};

            std::array<int, static_cast<size_t>(EvoTab::Count)> g_tab_selection{};
            std::array<std::string, static_cast<size_t>(EvoTab::Count)> g_tab_filters{};
            std::unordered_map<cradle::modules::Setting *, std::pair<evo::col_t, float>> g_color_cache;
            std::unordered_map<cradle::modules::Module *, int> g_keybind_ui_state;
            std::vector<std::string> g_friend_player_items;
            std::string g_friend_player_filter;
            int g_friend_player_selection = -1;
            int g_active_tab = 0;
            cradle::modules::Module *g_selected_module = nullptr;
            bool g_memory_view_active = false;
            MemoryPanelSection g_memory_view_section = MemoryPanelSection::None;

            struct CheatIndicatorEntry
            {
                const char *label;
                const char *module_name;
            };

            constexpr size_t kCheatIndicatorCount = 4;
            constexpr std::array<CheatIndicatorEntry, kCheatIndicatorCount> kCheatIndicatorEntries = {
                CheatIndicatorEntry{"aimbot", "aimbot"},
                CheatIndicatorEntry{"triggerbot", "triggerbot"},
                CheatIndicatorEntry{"silent aim", "silent aim"},
                CheatIndicatorEntry{"anti aim", "anti aim"}};

            struct MovementOverrideState
            {
                float custom_walkspeed = 16.0f;
                float custom_jumpheight = 7.5f;

                float default_walkspeed = 16.0f;
                float default_jumpheight = 7.5f;

                bool defaults_initialized = false;
                bool humanoid_ready = false;
                bool capture_next_defaults = false;

                bool lock_walkspeed = false;
                bool lock_jumpheight = false;

                bool enforce_walk_after_apply = false;
                bool enforce_jump_after_apply = false;

                double last_walk_enforce_time = 0.0;
                double last_jump_enforce_time = 0.0;

                bool last_walk_apply_success = false;
                bool last_jump_apply_success = false;

                float last_walk_apply_value = 16.0f;
                float last_jump_apply_value = 7.5f;

                float last_walk_apply_time = 0.0f;
                float last_jump_apply_time = 0.0f;
            };

            MovementOverrideState g_movement_override_state;

            constexpr float kMovementTolerance = 0.05f;
            constexpr double kMovementEnforceInterval = 0.05;

            constexpr double kGravityEnforceInterval = 0.05;
            constexpr float kDefaultGravity = 196.2f;
            constexpr double kLightingEnforceInterval = 0.05;
            constexpr float kLightingMaxFogDistance = 100000.0f;
            constexpr float kLightingClockMin = 0.0f;
            constexpr float kLightingClockMax = 24.0f;
            constexpr float kLightingLatitudeMin = -90.0f;
            constexpr float kLightingLatitudeMax = 90.0f;
            constexpr float kLightingExposureMin = -8.0f;
            constexpr float kLightingExposureMax = 8.0f;

            struct GravityOverrideState
            {
                float custom_gravity = kDefaultGravity;
                float default_gravity = kDefaultGravity;
                bool defaults_initialized = false;
                bool lock_gravity = false;
                bool workspace_ready = false;
                bool last_apply_success = false;
                float last_apply_value = kDefaultGravity;
                float last_apply_time = 0.0f;
                bool enforce_after_apply = false;
                double last_enforce_time = 0.0;
            };

            GravityOverrideState g_gravity_override_state;

            struct LightingOverrideState
            {
                cradle::engine::vector3 ambient{0.157f, 0.129f, 0.184f};
                cradle::engine::vector3 color_shift_bottom{0.0f, 0.0f, 0.0f};
                cradle::engine::vector3 color_shift_top{0.0f, 0.0f, 0.0f};
                cradle::engine::vector3 fog_color{0.753f, 0.753f, 0.753f};
                cradle::engine::vector3 outdoor_ambient{0.0f, 0.0f, 0.0f};
                float brightness = 4.6f;
                float fog_start = 0.0f;
                float fog_end = 1000.0f;
                float exposure_compensation = 0.0f;
                float clock_time = 12.0f;
                float geographic_latitude = 41.733f;

                cradle::engine::vector3 default_ambient{0.157f, 0.129f, 0.184f};
                cradle::engine::vector3 default_color_shift_bottom{0.0f, 0.0f, 0.0f};
                cradle::engine::vector3 default_color_shift_top{0.0f, 0.0f, 0.0f};
                cradle::engine::vector3 default_fog_color{0.753f, 0.753f, 0.753f};
                cradle::engine::vector3 default_outdoor_ambient{0.0f, 0.0f, 0.0f};
                float default_brightness = 4.6f;
                float default_fog_start = 0.0f;
                float default_fog_end = 1000.0f;
                float default_exposure_compensation = 0.0f;
                float default_clock_time = 12.0f;
                float default_geographic_latitude = 41.733f;

                bool defaults_initialized = false;
                bool lighting_ready = false;
                bool render_view_ready = false;
                bool last_apply_success = false;
                float last_apply_time = 0.0f;
                uintptr_t last_lighting_address = 0;
                bool lock_lighting = false;
                bool enforce_after_apply = false;
                double last_enforce_time = 0.0;

                float ambient_hue = 0.0f;
                float color_shift_bottom_hue = 0.0f;
                float color_shift_top_hue = 0.0f;
                float fog_color_hue = 0.0f;
                float outdoor_ambient_hue = 0.0f;

                bool pending_apply = false;
            };

            static bool nearly_equal(float a, float b, float eps);

            bool LightingVectorChanged(const cradle::engine::vector3 &before, const cradle::engine::vector3 &after)
            {
                return !nearly_equal(before.X, after.X, 0.0005f) ||
                       !nearly_equal(before.Y, after.Y, 0.0005f) ||
                       !nearly_equal(before.Z, after.Z, 0.0005f);
            }

            bool LightingFloatChanged(float before, float after, float epsilon = 0.0005f)
            {
                return !nearly_equal(before, after, epsilon);
            }

            void MarkLightingPendingApply(LightingOverrideState &state)
            {
                state.pending_apply = true;
            }

            LightingOverrideState g_lighting_override_state;

            static bool nearly_equal(float a, float b, float eps)
            {
                return std::fabs(a - b) <= eps;
            }

            void EnsureMovementDefaults(MovementOverrideState &state, const cradle::engine::Player &local_player)
            {
                if (!local_player.humanoid.is_valid())
                    return;

                auto humanoid_addr = local_player.humanoid.address;
                float current_walk = cradle::memory::read<float>(humanoid_addr + Offsets::Humanoid::Walkspeed);
                float current_jump = cradle::memory::read<float>(humanoid_addr + Offsets::Humanoid::JumpHeight);

                bool need_capture = state.capture_next_defaults || !state.defaults_initialized;
                if (!need_capture)
                    return;

                state.default_walkspeed = std::isfinite(current_walk) ? current_walk : 16.0f;
                state.default_jumpheight = std::isfinite(current_jump) ? current_jump : 7.5f;

                if (!state.defaults_initialized)
                {
                    state.custom_walkspeed = state.default_walkspeed;
                    state.custom_jumpheight = state.default_jumpheight;
                }

                state.defaults_initialized = true;
                state.capture_next_defaults = false;
            }

            bool ApplyWalkspeedOverride(MovementOverrideState &state,
                                        const cradle::engine::Player &local_player,
                                        float target_walk,
                                        bool force_write)
            {
                if (!local_player.humanoid.is_valid())
                {
                    state.last_walk_apply_success = false;
                    return false;
                }

                auto humanoid_addr = local_player.humanoid.address;
                float current_walk = cradle::memory::read<float>(humanoid_addr + Offsets::Humanoid::Walkspeed);
                float current_check = cradle::memory::read<float>(humanoid_addr + Offsets::Humanoid::WalkspeedCheck);

                bool need_walk = force_write || !nearly_equal(current_walk, target_walk, kMovementTolerance) || !nearly_equal(current_check, target_walk, kMovementTolerance);
                if (!need_walk)
                    return false;

                cradle::memory::write<float>(humanoid_addr + Offsets::Humanoid::Walkspeed, target_walk);
                cradle::memory::write<float>(humanoid_addr + Offsets::Humanoid::WalkspeedCheck, target_walk);

                float now = static_cast<float>(ImGui::GetTime());
                state.last_walk_apply_success = true;
                state.last_walk_apply_value = target_walk;
                state.last_walk_apply_time = now;
                return true;
            }

            bool ApplyJumpHeightOverride(MovementOverrideState &state,
                                         const cradle::engine::Player &local_player,
                                         float target_jump,
                                         bool force_write)
            {
                if (!local_player.humanoid.is_valid())
                {
                    state.last_jump_apply_success = false;
                    return false;
                }

                auto humanoid_addr = local_player.humanoid.address;
                float current_jump = cradle::memory::read<float>(humanoid_addr + Offsets::Humanoid::JumpHeight);

                bool need_jump = force_write || !nearly_equal(current_jump, target_jump, kMovementTolerance);
                if (!need_jump)
                    return false;

                cradle::memory::write<float>(humanoid_addr + Offsets::Humanoid::JumpHeight, target_jump);
                cradle::memory::write<float>(humanoid_addr + Offsets::Humanoid::JumpPower, std::max(target_jump * 3.5f, target_jump));

                float now = static_cast<float>(ImGui::GetTime());
                state.last_jump_apply_success = true;
                state.last_jump_apply_value = target_jump;
                state.last_jump_apply_time = now;
                return true;
            }

            void TickMovementOverrides()
            {
                auto &state = g_movement_override_state;
                auto local_player = cradle::engine::PlayerCache::get_local_player();
                state.humanoid_ready = local_player.humanoid.is_valid();

                if (!state.humanoid_ready)
                {
                    state.defaults_initialized = false;
                    state.enforce_walk_after_apply = false;
                    state.enforce_jump_after_apply = false;
                    state.capture_next_defaults = false;
                    return;
                }

                EnsureMovementDefaults(state, local_player);

                double now = ImGui::GetTime();
                bool enforce_walk = state.lock_walkspeed || state.enforce_walk_after_apply;
                if (enforce_walk && (now - state.last_walk_enforce_time) >= kMovementEnforceInterval)
                {
                    bool wrote_walk = ApplyWalkspeedOverride(state, local_player, state.custom_walkspeed, false);
                    state.enforce_walk_after_apply = state.lock_walkspeed;
                    state.last_walk_enforce_time = now;
                    if (!wrote_walk && !state.lock_walkspeed)
                        state.enforce_walk_after_apply = false;
                }

                bool enforce_jump = state.lock_jumpheight || state.enforce_jump_after_apply;
                if (enforce_jump && (now - state.last_jump_enforce_time) >= kMovementEnforceInterval)
                {
                    bool wrote_jump = ApplyJumpHeightOverride(state, local_player, state.custom_jumpheight, false);
                    state.enforce_jump_after_apply = state.lock_jumpheight;
                    state.last_jump_enforce_time = now;
                    if (!wrote_jump && !state.lock_jumpheight)
                        state.enforce_jump_after_apply = false;
                }
            }

            bool TryReadWorkspaceGravity(const cradle::engine::Instance &workspace, float &gravity)
            {
                if (!workspace.is_valid())
                    return false;

                auto gravity_container = cradle::memory::read<std::uintptr_t>(workspace.address + Offsets::Workspace::GravityContainer);
                if (!cradle::memory::IsValid(gravity_container))
                    return false;

                float current = cradle::memory::read<float>(gravity_container + Offsets::Workspace::Gravity);
                if (!std::isfinite(current))
                    return false;

                gravity = current;
                return true;
            }

            bool TryWriteWorkspaceGravity(const cradle::engine::Instance &workspace, float gravity)
            {
                if (!workspace.is_valid())
                    return false;

                auto gravity_container = cradle::memory::read<std::uintptr_t>(workspace.address + Offsets::Workspace::GravityContainer);
                if (!cradle::memory::IsValid(gravity_container))
                    return false;

                cradle::memory::write<float>(gravity_container + Offsets::Workspace::Gravity, gravity);
                return true;
            }

            void EnsureGravityDefaults(GravityOverrideState &state, const cradle::engine::Instance &workspace)
            {
                if (state.defaults_initialized)
                    return;

                float current = 0.0f;
                if (TryReadWorkspaceGravity(workspace, current))
                {
                    state.default_gravity = current;
                    state.custom_gravity = current;
                    state.defaults_initialized = true;
                }
            }

            void TickGravityOverride()
            {
                auto &state = g_gravity_override_state;
                auto dm_instance = cradle::engine::DataModel::get_instance();
                auto workspace = dm_instance.get_workspace();

                state.workspace_ready = workspace.is_valid();
                if (!state.workspace_ready)
                {
                    state.defaults_initialized = false;
                    state.enforce_after_apply = false;
                    return;
                }

                EnsureGravityDefaults(state, workspace);

                double now = ImGui::GetTime();
                bool should_enforce = state.lock_gravity || state.enforce_after_apply;
                if (should_enforce && (now - state.last_enforce_time) >= kGravityEnforceInterval)
                {
                    if (TryWriteWorkspaceGravity(workspace, state.custom_gravity))
                    {
                        state.last_apply_success = true;
                        state.last_apply_value = state.custom_gravity;
                        state.last_apply_time = static_cast<float>(now);
                        state.enforce_after_apply = state.lock_gravity;
                    }
                    else
                    {
                        state.last_apply_success = false;
                        state.enforce_after_apply = false;
                    }

                    state.last_enforce_time = now;
                }
            }

            cradle::engine::vector3 ClampLightingColor(const cradle::engine::vector3 &color)
            {
                return cradle::engine::vector3(
                    std::clamp(color.X, 0.0f, 1.0f),
                    std::clamp(color.Y, 0.0f, 1.0f),
                    std::clamp(color.Z, 0.0f, 1.0f));
            }

            bool IsFiniteColor(const cradle::engine::vector3 &color)
            {
                return std::isfinite(color.X) && std::isfinite(color.Y) && std::isfinite(color.Z);
            }

            evo::col_t Color3ToEvoColor(const cradle::engine::vector3 &color)
            {
                return evo::col_t(
                    static_cast<int>(std::clamp(color.X, 0.0f, 1.0f) * 255.0f),
                    static_cast<int>(std::clamp(color.Y, 0.0f, 1.0f) * 255.0f),
                    static_cast<int>(std::clamp(color.Z, 0.0f, 1.0f) * 255.0f),
                    255);
            }

            cradle::engine::vector3 EvoColorToColor3(const evo::col_t &color)
            {
                return cradle::engine::vector3(
                    color.r / 255.0f,
                    color.g / 255.0f,
                    color.b / 255.0f);
            }

            bool TryReadLightingColor(const cradle::engine::Instance &lighting, uintptr_t offset, cradle::engine::vector3 &color)
            {
                if (!lighting.is_valid())
                    return false;

                auto value = cradle::memory::read<cradle::engine::vector3>(lighting.address + offset);
                if (!IsFiniteColor(value))
                    return false;

                color = ClampLightingColor(value);
                return true;
            }

            bool TryWriteLightingColor(const cradle::engine::Instance &lighting, uintptr_t offset, cradle::engine::vector3 color)
            {
                if (!lighting.is_valid())
                    return false;

                if (!IsFiniteColor(color))
                    return false;

                auto clamped = ClampLightingColor(color);
                return cradle::memory::write<cradle::engine::vector3>(lighting.address + offset, clamped);
            }

            bool TryReadLightingBrightness(const cradle::engine::Instance &lighting, float &brightness)
            {
                if (!lighting.is_valid())
                    return false;

                float value = cradle::memory::read<float>(lighting.address + Offsets::Lighting::Brightness);
                if (!std::isfinite(value))
                    return false;
                brightness = value;
                return true;
            }

            bool TryWriteLightingBrightness(const cradle::engine::Instance &lighting, float brightness)
            {
                if (!lighting.is_valid() || !std::isfinite(brightness))
                    return false;
                float clamped = std::clamp(brightness, 0.0f, 10.0f);
                return cradle::memory::write<float>(lighting.address + Offsets::Lighting::Brightness, clamped);
            }

            bool TryReadLightingFloat(const cradle::engine::Instance &lighting, uintptr_t offset, float &value_out)
            {
                if (!lighting.is_valid())
                    return false;

                float value = cradle::memory::read<float>(lighting.address + offset);
                if (!std::isfinite(value))
                    return false;

                value_out = value;
                return true;
            }

            bool TryWriteLightingFloat(const cradle::engine::Instance &lighting, uintptr_t offset, float value)
            {
                if (!lighting.is_valid() || !std::isfinite(value))
                    return false;
                return cradle::memory::write<float>(lighting.address + offset, value);
            }

            void EnsureLightingDefaults(LightingOverrideState &state, const cradle::engine::Instance &lighting)
            {
                if (state.last_lighting_address != lighting.address)
                {
                    state.defaults_initialized = false;
                    state.last_lighting_address = lighting.address;
                }

                if (state.defaults_initialized)
                    return;

                cradle::engine::vector3 ambient{};
                cradle::engine::vector3 cs_bottom{};
                cradle::engine::vector3 cs_top{};
                cradle::engine::vector3 fog{};
                cradle::engine::vector3 outdoor{};
                float brightness = 0.0f;
                float fog_start = 0.0f;
                float fog_end = 0.0f;
                float exposure = 0.0f;
                float clock_time = 0.0f;
                float latitude = 0.0f;

                bool ok = true;
                ok &= TryReadLightingColor(lighting, Offsets::Lighting::Ambient, ambient);
                ok &= TryReadLightingColor(lighting, Offsets::Lighting::ColorShift_Bottom, cs_bottom);
                ok &= TryReadLightingColor(lighting, Offsets::Lighting::ColorShift_Top, cs_top);
                ok &= TryReadLightingColor(lighting, Offsets::Lighting::FogColor, fog);
                ok &= TryReadLightingColor(lighting, Offsets::Lighting::OutdoorAmbient, outdoor);
                ok &= TryReadLightingBrightness(lighting, brightness);
                ok &= TryReadLightingFloat(lighting, Offsets::Lighting::FogStart, fog_start);
                ok &= TryReadLightingFloat(lighting, Offsets::Lighting::FogEnd, fog_end);
                ok &= TryReadLightingFloat(lighting, Offsets::Lighting::ExposureCompensation, exposure);
                ok &= TryReadLightingFloat(lighting, Offsets::Lighting::ClockTime, clock_time);
                ok &= TryReadLightingFloat(lighting, Offsets::Lighting::GeographicLatitude, latitude);

                if (!ok)
                    return;

                state.default_ambient = ClampLightingColor(ambient);
                state.default_color_shift_bottom = ClampLightingColor(cs_bottom);
                state.default_color_shift_top = ClampLightingColor(cs_top);
                state.default_fog_color = ClampLightingColor(fog);
                state.default_outdoor_ambient = ClampLightingColor(outdoor);
                state.default_brightness = std::clamp(brightness, 0.0f, 10.0f);
                state.default_fog_start = std::clamp(fog_start, 0.0f, kLightingMaxFogDistance);
                float fog_end_clamped = std::clamp(fog_end, 0.0f, kLightingMaxFogDistance);
                state.default_fog_end = std::max(fog_end_clamped, state.default_fog_start + 0.1f);
                state.default_exposure_compensation = std::clamp(exposure, kLightingExposureMin, kLightingExposureMax);
                state.default_clock_time = std::clamp(clock_time, kLightingClockMin, kLightingClockMax);
                state.default_geographic_latitude = std::clamp(latitude, kLightingLatitudeMin, kLightingLatitudeMax);

                state.ambient = state.default_ambient;
                state.color_shift_bottom = state.default_color_shift_bottom;
                state.color_shift_top = state.default_color_shift_top;
                state.fog_color = state.default_fog_color;
                state.brightness = state.default_brightness;
                state.outdoor_ambient = state.default_outdoor_ambient;
                state.fog_start = state.default_fog_start;
                state.fog_end = state.default_fog_end;
                state.exposure_compensation = state.default_exposure_compensation;
                state.clock_time = state.default_clock_time;
                state.geographic_latitude = state.default_geographic_latitude;

                state.defaults_initialized = true;
            }

            bool ApplyLightingOverrides(LightingOverrideState &state,
                                         const cradle::engine::Instance &lighting,
                                         const cradle::engine::Instance &render_view)
            {
                if (!lighting.is_valid() || !render_view.is_valid())
                    return false;

                state.ambient = ClampLightingColor(state.ambient);
                state.color_shift_bottom = ClampLightingColor(state.color_shift_bottom);
                state.color_shift_top = ClampLightingColor(state.color_shift_top);
                state.fog_color = ClampLightingColor(state.fog_color);
                state.outdoor_ambient = ClampLightingColor(state.outdoor_ambient);
                state.brightness = std::clamp(state.brightness, 0.0f, 10.0f);
                state.fog_start = std::clamp(state.fog_start, 0.0f, kLightingMaxFogDistance);
                state.fog_end = std::clamp(state.fog_end, state.fog_start + 0.1f, kLightingMaxFogDistance);
                state.exposure_compensation = std::clamp(state.exposure_compensation, kLightingExposureMin, kLightingExposureMax);
                state.clock_time = std::clamp(state.clock_time, kLightingClockMin, kLightingClockMax);
                state.geographic_latitude = std::clamp(state.geographic_latitude, kLightingLatitudeMin, kLightingLatitudeMax);

                bool ok = true;
                ok &= TryWriteLightingColor(lighting, Offsets::Lighting::Ambient, state.ambient);
                ok &= TryWriteLightingColor(lighting, Offsets::Lighting::ColorShift_Bottom, state.color_shift_bottom);
                ok &= TryWriteLightingColor(lighting, Offsets::Lighting::ColorShift_Top, state.color_shift_top);
                ok &= TryWriteLightingColor(lighting, Offsets::Lighting::FogColor, state.fog_color);
                ok &= TryWriteLightingColor(lighting, Offsets::Lighting::OutdoorAmbient, state.outdoor_ambient);
                ok &= TryWriteLightingBrightness(lighting, state.brightness);
                ok &= TryWriteLightingFloat(lighting, Offsets::Lighting::FogStart, state.fog_start);
                ok &= TryWriteLightingFloat(lighting, Offsets::Lighting::FogEnd, state.fog_end);
                ok &= TryWriteLightingFloat(lighting, Offsets::Lighting::ExposureCompensation, state.exposure_compensation);
                ok &= TryWriteLightingFloat(lighting, Offsets::Lighting::ClockTime, state.clock_time);
                ok &= TryWriteLightingFloat(lighting, Offsets::Lighting::GeographicLatitude, state.geographic_latitude);

                state.last_apply_time = static_cast<float>(ImGui::GetTime());

                bool invalidated = false;
                if (ok)
                {
                    invalidated = cradle::engine::lighting::invalidate(render_view);
                }

                state.last_apply_success = ok && invalidated;
                return state.last_apply_success;
            }

            void TickLightingOverrides()
            {
                auto &state = g_lighting_override_state;
                auto dm_instance = cradle::engine::DataModel::get_instance();
                auto lighting = dm_instance.get_lighting();
                auto render_view = dm_instance.get_render_view();

                state.lighting_ready = lighting.is_valid();
                state.render_view_ready = render_view.is_valid();

                if (!state.lighting_ready)
                {
                    state.defaults_initialized = false;
                    state.enforce_after_apply = false;
                    state.last_lighting_address = 0;
                    return;
                }

                EnsureLightingDefaults(state, lighting);

                if (!state.render_view_ready)
                {
                    state.enforce_after_apply = false;
                    return;
                }

                double now = ImGui::GetTime();
                bool needs_apply = state.pending_apply;
                bool enforce = state.lock_lighting || state.enforce_after_apply || needs_apply;
                if (!enforce)
                    return;

                if ((now - state.last_enforce_time) < kLightingEnforceInterval)
                    return;

                bool applied = ApplyLightingOverrides(state, lighting, render_view);
                state.last_enforce_time = now;

                if (applied && needs_apply)
                    state.pending_apply = false;

                if (applied)
                {
                    state.enforce_after_apply = state.lock_lighting;
                }
                else
                {
                    state.enforce_after_apply = state.lock_lighting || state.pending_apply;
                }
            }

[[maybe_unused]] const char *GetKeyName(int vk)
                                {
                                    static char name[64];
                                    if (vk == 0)
                                        return "none";

                                    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
                                    {
                                        name[0] = static_cast<char>(vk);
                                        name[1] = '\0';
                                        return name;
                                    }

                                    int scan_code = MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_VSC) << 16;
                                    if (GetKeyNameTextA(scan_code, name, sizeof(name)))
                                        return name;

                                    return "unknown";
                                }

                                evo::col_t ToEvoColor(const float color[4])
                                {
                                    return evo::col_t(
                                        static_cast<int>(std::clamp(color[0], 0.0f, 1.0f) * 255.0f),
                                        static_cast<int>(std::clamp(color[1], 0.0f, 1.0f) * 255.0f),
                                        static_cast<int>(std::clamp(color[2], 0.0f, 1.0f) * 255.0f),
                                        static_cast<int>(std::clamp(color[3], 0.0f, 1.0f) * 255.0f));
                                }

                                void StoreColorBack(cradle::modules::Setting &setting, const evo::col_t &color)
                                {
                                    setting.value.color_val[0] = color.r / 255.0f;
                                    setting.value.color_val[1] = color.g / 255.0f;
                                    setting.value.color_val[2] = color.b / 255.0f;
                                    setting.value.color_val[3] = color.a / 255.0f;
                                }

                                void RenderColorSettingControl(evo::child_t *child, cradle::modules::Setting &setting)
                                {
                                    auto &cache_entry = g_color_cache[&setting];
                                    cache_entry.first = ToEvoColor(setting.value.color_val);
                                    if (cache_entry.second <= 0.0f)
                                        cache_entry.second = cache_entry.first.hue();
                                    child->make_text(setting.name);
                                    child->make_colorpicker(&cache_entry.first, &cache_entry.second);
                                    float before[4] = {
                                        setting.value.color_val[0],
                                        setting.value.color_val[1],
                                        setting.value.color_val[2],
                                        setting.value.color_val[3]};
                                    StoreColorBack(setting, cache_entry.first);
                                    bool changed = false;
                                    for (int i = 0; i < 4; ++i)
                                    {
                                        if (std::fabs(setting.value.color_val[i] - before[i]) > 0.0001f)
                                        {
                                            changed = true;
                                            break;
                                        }
                                    }
                                    if (changed)
                                        cradle::config::ConfigManager::mark_dirty();
                                }

                                cradle::modules::KeybindMode KeybindModeFromUiSelection(int selection)
                                {
                                    return (selection == 1 || selection == 3)
                                               ? cradle::modules::KeybindMode::HOLD
                                               : cradle::modules::KeybindMode::TOGGLE;
                                }

                                int DefaultUiSelectionForMode(cradle::modules::KeybindMode mode)
                                {
                                    return mode == cradle::modules::KeybindMode::HOLD ? 1 : 2; // hold on / toggle
                                }

                                int GetKeybindUiSelection(cradle::modules::Module *module)
                                {
                                    auto it = g_keybind_ui_state.find(module);
                                    if (it != g_keybind_ui_state.end())
                                    {
                                        if (KeybindModeFromUiSelection(it->second) != module->get_keybind_mode())
                                        {
                                            it->second = DefaultUiSelectionForMode(module->get_keybind_mode());
                                        }
                                        return it->second;
                                    }
                                    int selection = DefaultUiSelectionForMode(module->get_keybind_mode());
                                    g_keybind_ui_state[module] = selection;
                                    return selection;
                                }

                                void StoreKeybindUiSelection(cradle::modules::Module *module, int selection)
                                {
                                    selection = std::clamp(selection, 0, 3);
                                    g_keybind_ui_state[module] = selection;
                                }

                                EvoTab DetectTab(cradle::modules::Module *module)
                                {
                                    std::string lower = module->get_name();
                                    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                                    if (lower.find("esp") != std::string::npos)
                                        return EvoTab::Esp;
                                    if (lower.find("friend") != std::string::npos)
                                        return EvoTab::Esp;
                                    if (lower.find("color") != std::string::npos || lower.find("visual") != std::string::npos)
                                        return EvoTab::Esp;
                                    if (lower.find("profiling") != std::string::npos)
                                        return EvoTab::Esp;
                                    if (lower.find("aim") != std::string::npos || lower.find("trigger") != std::string::npos)
                                        return EvoTab::Aimbot;
                                    return EvoTab::Misc;
                                }

                                void RenderModulePanel(evo::child_t *child, cradle::modules::Module *module);
                                void RenderEspSettings(evo::child_t *child, cradle::modules::Module *module);
                                void RenderMemoryPanel(evo::child_t *child, MemoryPanelSection section);
                                void RenderWalkspeedMemorySection(evo::child_t *child);
                                void RenderJumpHeightMemorySection(evo::child_t *child);
                                void RenderGravityMemorySection(evo::child_t *child);
                                void RenderLightingMemorySection(evo::child_t *child);
                                void RenderVisualColorPanel(evo::child_t *child);
                                void RenderProfilingPanel(evo::child_t *child, cradle::modules::Module *module);
                                void RenderConfigTab(evo::window_t *window);
                                void RenderTabContents(evo::window_t *window, EvoTab tab);
                                void RenderCheatIndicator();
                                void TickLightingOverrides();

                                void RenderEvoMenuWindow(Overlay &overlay)
                                {
                                    std::vector<std::string> tab_icons;
                                    tab_icons.reserve(kTabs.size());
                                    for (const auto &tab : kTabs)
                                        tab_icons.emplace_back(tab.icon);

                                    g_active_tab = std::clamp(g_active_tab, 0, static_cast<int>(kTabs.size() - 1));
                                    evo::theme::selected_tab = g_active_tab;
                                    int previous_tab = g_active_tab;

                                    auto window = new evo::window_t("mossad.is", &evo::theme::menu_spawn, evo::theme::menu_size, tab_icons, &g_active_tab);
                                    {
                                        RenderTabContents(window, static_cast<EvoTab>(g_active_tab));
                                    }
                                    delete window;

                                    if (g_active_tab != previous_tab)
                                        evo::reset_all_popups();

                                    evo::externals::_ext_b->begin();
                                    evo::externals::_ext_b_p->begin_popup();
                                }

                                void RenderTabContents(evo::window_t *window, EvoTab tab)
                                {
                                    int tab_index = static_cast<int>(tab);
                                    if (tab == EvoTab::Config)
                                    {
                                        RenderConfigTab(window);
                                        return;
                                    }
                                    auto &manager = cradle::modules::ModuleManager::get_instance();
                                    auto modules = manager.get_all_modules();

                                    std::vector<ModuleListEntry> entries;
                                    entries.reserve(modules.size() + 1);

                                    if (tab == EvoTab::Misc)
                                    {
                                        entries.push_back({"Walkspeed override", nullptr, MemoryPanelSection::Walkspeed});
                                        entries.push_back({"Jump height override", nullptr, MemoryPanelSection::JumpHeight});
                                        entries.push_back({"Gravity override", nullptr, MemoryPanelSection::Gravity});
                                        entries.push_back({"Lighting reference overrides", nullptr, MemoryPanelSection::Lighting});
                                    }

                                    for (auto *mod : modules)
                                    {
                                        if (DetectTab(mod) != tab)
                                            continue;

                                        std::string label = mod->get_name();
                                        if (!label.empty())
                                            label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
                                        entries.push_back({label, mod, MemoryPanelSection::None});
                                    }

                                    int &selected_idx = g_tab_selection[tab_index];
                                    if (entries.empty())
                                    {
                                        selected_idx = -1;
                                    }
                                    else
                                    {
                                        if (selected_idx < 0 || selected_idx >= static_cast<int>(entries.size()))
                                            selected_idx = 0;
                                    }

                                    auto module_child = new evo::child_t("modules", 0, evo::child_pos_t::_top_left, evo::child_size_t::_full, tab_index);
                                    {
                                        window->make_child(module_child);
                                        if (entries.empty())
                                        {
                                            module_child->make_text("no modules available");
                                        }
                                        else
                                        {
                                            std::vector<std::string> labels;
                                            labels.reserve(entries.size());
                                            for (auto &entry : entries)
                                                labels.emplace_back(entry.label);

                                            module_child->make_listbox("##module_list", &selected_idx, labels, g_tab_filters[tab_index]);
                                            module_child->enable_scrolling();
                                        }
                                    }
                                    delete module_child;

                                    g_memory_view_active = false;
                                    g_memory_view_section = MemoryPanelSection::None;
                                    g_selected_module = nullptr;

                                    if (!entries.empty() && selected_idx >= 0 && selected_idx < static_cast<int>(entries.size()))
                                    {
                                        const auto &entry = entries[selected_idx];
                                        if (entry.memory_section != MemoryPanelSection::None)
                                        {
                                            g_memory_view_active = true;
                                            g_memory_view_section = entry.memory_section;
                                        }
                                        else
                                            g_selected_module = entry.module;
                                    }

                                    auto settings_child = new evo::child_t("details", 1, evo::child_pos_t::_top_right, evo::child_size_t::_full, tab_index);
                                    {
                                        window->make_child(settings_child);
                                        if (g_memory_view_active && tab == EvoTab::Misc)
                                        {
                                            RenderMemoryPanel(settings_child, g_memory_view_section);
                                        }
                                        else if (g_selected_module)
                                        {
                                            RenderModulePanel(settings_child, g_selected_module);
                                        }
                                        else
                                        {
                                            settings_child->make_text("select a module to configure");
                                        }
                                    }
                                    delete settings_child;
                                }

                                void RenderModulePanel(evo::child_t *child, cradle::modules::Module *module)
                                {
                                    child->make_text(module->get_name());
                                    if (!module->get_description().empty())
                                        child->make_text(module->get_description());

                                    const bool is_esp = module->get_name() == "esp";
                                    const bool is_aimbot = module->get_name() == "aimbot";
                                    const bool is_anti_aim = module->get_name() == "anti aim";
                                    const bool is_friends = module->get_name() == "friends";
                                    const bool is_visual_colors = module->get_name() == "visual colors";
                                    const bool is_profiling = module->get_name() == "profiling";

                                    if (is_esp || is_friends || is_visual_colors || is_profiling)
                                    {
                                        if (!module->is_enabled())
                                            module->set_enabled(true);
                                    }
                                    else if (!is_aimbot)
                                    {
                                        bool enabled = module->is_enabled();
                                        child->make_checkbox("enabled", &enabled);
                                        if (enabled != module->is_enabled())
                                            module->set_enabled(enabled);
                                    }

                                    if (!(is_esp || is_friends || is_visual_colors || is_profiling))
                                    {
                                        int key_code = module->get_keybind();
                                        int key_mode = GetKeybindUiSelection(module);
                                        child->make_keybind(&key_code, &key_mode);
                                        if (key_code != module->get_keybind())
                                        {
                                            module->set_keybind(key_code);
                                            cradle::modules::ModuleManager::get_instance().update_keybinds();
                                        }
                                        StoreKeybindUiSelection(module, key_mode);
                                        auto desired_mode = KeybindModeFromUiSelection(key_mode);
                                        if (desired_mode != module->get_keybind_mode())
                                            module->set_keybind_mode(desired_mode);
                                    }
                                    else
                                    {
                                        if (module->get_keybind() != 0)
                                        {
                                            module->set_keybind(0);
                                            cradle::modules::ModuleManager::get_instance().update_keybinds();
                                        }
                                        if (module->get_keybind_mode() != cradle::modules::KeybindMode::TOGGLE)
                                        {
                                            module->set_keybind_mode(cradle::modules::KeybindMode::TOGGLE);
                                        }
                                        if (is_esp)
                                        {
                                            child->make_text("ESP stays enabled automatically; use the feature toggles below.");
                                        }
                                        else if (is_friends)
                                        {
                                            child->make_text("Friends stay enabled automatically; manage the list below.");
                                        }
                                        else if (is_visual_colors)
                                        {
                                            child->make_text("Visual colors stay enabled automatically; adjust the palette below.");
                                        }
                                        else if (is_profiling)
                                        {
                                            child->make_text("Profiling stays enabled automatically; toggle overlays below.");
                                        }
                                    }

                                    if (is_aimbot)
                                    {
                                        child->make_text("Aimbot runs only while its keybind is active.");
                                    }

                                    if (is_anti_aim)
                                    {
                                        if (auto *anti_module = dynamic_cast<cradle::modules::AntiAimModule *>(module))
                                        {
                                            int desync_key_code = anti_module->get_desync_hotkey();
                                            int desync_key_mode = DefaultUiSelectionForMode(anti_module->get_desync_hotkey_mode());
                                            child->make_text("Desync hotkey (leave empty for always-on).");
                                            child->make_keybind(&desync_key_code, &desync_key_mode);
                                            if (desync_key_code != anti_module->get_desync_hotkey())
                                                anti_module->set_desync_hotkey(desync_key_code);
                                            auto desired_mode = KeybindModeFromUiSelection(desync_key_mode);
                                            if (desired_mode != anti_module->get_desync_hotkey_mode())
                                                anti_module->set_desync_hotkey_mode(desired_mode);
                                            child->make_text("Use hold for momentary desync or toggle to latch it.");
                                        }
                                    }

                                    if (is_friends)
                                    {
                                        auto players_snapshot = cradle::engine::PlayerCache::get_players();
                                        std::unordered_set<std::string> seen;
                                        g_friend_player_items.clear();
                                        if (players_snapshot && !players_snapshot->empty())
                                        {
                                            g_friend_player_items.reserve(players_snapshot->size());
                                            for (const auto &p : *players_snapshot)
                                            {
                                                if (p.name.empty())
                                                    continue;
                                                std::string lower = p.name;
                                                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                                                if (seen.insert(lower).second)
                                                    g_friend_player_items.push_back(p.name);
                                            }
                                        }
                                        std::sort(g_friend_player_items.begin(), g_friend_player_items.end(), [](const std::string &a, const std::string &b) {
                                            return _stricmp(a.c_str(), b.c_str()) < 0;
                                        });

                                        const bool has_player_choices = !g_friend_player_items.empty();
                                        if (!has_player_choices)
                                        {
                                            g_friend_player_selection = -1;
                                            g_friend_player_items.push_back("no players cached");
                                        }
                                        else if (g_friend_player_selection < 0 || g_friend_player_selection >= static_cast<int>(g_friend_player_items.size()))
                                        {
                        g_friend_player_selection = 0;
                                        }

                                        child->make_text("Friends (aimbot ignores these players)");
                                        child->make_listbox("players##friend_manager", &g_friend_player_selection, g_friend_player_items, g_friend_player_filter);

                                        child->make_button("toggle friend", [has_player_choices]() {
                                            if (!has_player_choices)
                                                return;
                                            if (g_friend_player_selection < 0 || g_friend_player_selection >= static_cast<int>(g_friend_player_items.size()))
                                                return;
                                            const std::string &name = g_friend_player_items[g_friend_player_selection];
                                            friends::toggle_friend(name);
                                            cradle::config::ConfigManager::mark_dirty();
                                        });

                                        child->make_button("clear friends", []() {
                                            friends::clear();
                                            cradle::config::ConfigManager::mark_dirty();
                                        });

                                        auto friend_names = friends::get_friends();
                                        if (friend_names.empty())
                                        {
                                            child->make_text("current friends: none");
                                        }
                                        else
                                        {
                                            child->make_text("current friends:");
                                            for (const auto &name : friend_names)
                                            {
                                                child->make_text(std::string(" - ") + name);
                                            }
                                        }

                                        return;
                                    }

                                    if (is_visual_colors)
                                    {
                                        RenderVisualColorPanel(child);
                                        return;
                                    }

                                    if (is_profiling)
                                    {
                                        RenderProfilingPanel(child, module);
                                        return;
                                    }

                                    if (is_esp)
                                    {
                                        RenderEspSettings(child, module);
                                        return;
                                    }

                                    for (auto &setting : module->get_settings())
                                    {
                                        if (is_esp)
                                        {
                                            if (setting.name == "esp refresh ms" ||
                                                setting.name == "esp refresh limit" ||
                                                setting.name == "wallcheck debug overlay")
                                            {
                                                continue;
                                            }
                                        }
                                        switch (setting.type)
                                        {
                                        case cradle::modules::SettingType::BOOL:
                                        {
                                            bool before = setting.value.bool_val;
                                            child->make_checkbox(setting.name, &setting.value.bool_val);
                                            if (before != setting.value.bool_val)
                                                cradle::config::ConfigManager::mark_dirty();
                                            break;
                                        }
                                        case cradle::modules::SettingType::INT:
                                        {
                                            if (is_aimbot && setting.name == "target priority mode")
                                            {
                                                static const std::vector<std::string> kTargetPriorityLabels = {
                                                    "Closest to FOV center",
                                                    "Closest by distance",
                                                    "Lowest health"
                                                };
                                                int before = setting.value.int_val;
                                                if (setting.value.int_val < setting.range.int_range.min || setting.value.int_val > setting.range.int_range.max)
                                                    setting.value.int_val = setting.range.int_range.min;
                                                child->make_dropdown("target priority", &setting.value.int_val, kTargetPriorityLabels);
                                                if (before != setting.value.int_val)
                                                    cradle::config::ConfigManager::mark_dirty();
                                            }
                                            else if (is_anti_aim && setting.name == "spinbot mode")
                                            {
                                                static const std::vector<std::string> kSpinbotModes = {
                                                    "Smooth spin",
                                                    "Jitter flip",
                                                    "Position pulse"
                                                };
                                                int before = setting.value.int_val;
                                                if (setting.value.int_val < setting.range.int_range.min || setting.value.int_val > setting.range.int_range.max)
                                                    setting.value.int_val = setting.range.int_range.min;
                                                child->make_dropdown("spinbot mode", &setting.value.int_val, kSpinbotModes);
                                                if (before != setting.value.int_val)
                                                    cradle::config::ConfigManager::mark_dirty();
                                            }
                                            else
                                            {
                                                int before = setting.value.int_val;
                                                child->make_slider_int(setting.name, &setting.value.int_val, setting.range.int_range.min, setting.range.int_range.max, "");
                                                if (before != setting.value.int_val)
                                                    cradle::config::ConfigManager::mark_dirty();
                                            }
                                            break;
                                        }
                                        case cradle::modules::SettingType::FLOAT:
                                        {
                                            float before = setting.value.float_val;
                                            child->make_slider_float(setting.name, &setting.value.float_val, setting.range.float_range.min, setting.range.float_range.max, "");
                                            if (before != setting.value.float_val)
                                                cradle::config::ConfigManager::mark_dirty();
                                            break;
                                        }
                                        case cradle::modules::SettingType::COLOR:
                                        {
                                            if (is_esp || is_aimbot)
                                                break;
                                            RenderColorSettingControl(child, setting);
                                            break;
                                        }
                                        }
                                    }
                                }

                                void RenderEspSettings(evo::child_t *child, cradle::modules::Module *module)
                                {
                                    constexpr int kGeneralPopupId = 5;
                                    constexpr int kPerformancePopupId = 6;

                                    std::vector<std::function<void(evo::popup_t *)>> general_bindings;
                                    std::vector<std::function<void(evo::popup_t *)>> performance_bindings;
                                    std::vector<std::function<void()>> dirty_checks;

                                    auto bind_checkbox = [&](const char *setting_name, std::vector<std::function<void(evo::popup_t *)>> &bucket) {
                                        auto *setting = module->get_setting(setting_name);
                                        if (!setting || setting->type != cradle::modules::SettingType::BOOL)
                                            return;
                                        bool before = setting->value.bool_val;
                                        bucket.emplace_back([setting](evo::popup_t *popup) {
                                            popup->bind_checkbox(setting->name, &setting->value.bool_val);
                                        });
                                        dirty_checks.emplace_back([before, setting]() {
                                            if (before != setting->value.bool_val)
                                                cradle::config::ConfigManager::mark_dirty();
                                        });
                                    };

                                    auto bind_slider_int = [&](const char *setting_name, std::vector<std::function<void(evo::popup_t *)>> &bucket, const char *suffix = "") {
                                        auto *setting = module->get_setting(setting_name);
                                        if (!setting || setting->type != cradle::modules::SettingType::INT)
                                            return;
                                        int before = setting->value.int_val;
                                        int min = setting->range.int_range.min;
                                        int max = setting->range.int_range.max;
                                        std::string suffix_copy = suffix ? suffix : "";
                                        bucket.emplace_back([setting, min, max, suffix_copy](evo::popup_t *popup) {
                                            popup->bind_slider_int(setting->name, &setting->value.int_val, min, max, suffix_copy);
                                        });
                                        dirty_checks.emplace_back([before, setting]() {
                                            if (before != setting->value.int_val)
                                                cradle::config::ConfigManager::mark_dirty();
                                        });
                                    };

                                    auto bind_slider_float = [&](const char *setting_name, std::vector<std::function<void(evo::popup_t *)>> &bucket, const char *suffix = "") {
                                        auto *setting = module->get_setting(setting_name);
                                        if (!setting || setting->type != cradle::modules::SettingType::FLOAT)
                                            return;
                                        float before = setting->value.float_val;
                                        int min = static_cast<int>(std::floor(setting->range.float_range.min));
                                        int max = static_cast<int>(std::ceil(setting->range.float_range.max));
                                        if (min == max)
                                            max = min + 1;
                                        std::string suffix_copy = suffix ? suffix : "";
                                        bucket.emplace_back([setting, min, max, suffix_copy](evo::popup_t *popup) {
                                            popup->bind_slider_float(setting->name, &setting->value.float_val, min, max, suffix_copy);
                                        });
                                        dirty_checks.emplace_back([before, setting]() {
                                            if (std::fabs(before - setting->value.float_val) > 0.0001f)
                                                cradle::config::ConfigManager::mark_dirty();
                                        });
                                    };

                                    auto bind_dropdown = [&](const char *setting_name, const std::vector<std::string> &choices, std::vector<std::function<void(evo::popup_t *)>> &bucket) {
                                        auto *setting = module->get_setting(setting_name);
                                        if (!setting || setting->type != cradle::modules::SettingType::INT)
                                            return;
                                        int before = setting->value.int_val;
                                        std::vector<std::string> labels = choices;
                                        bucket.emplace_back([setting, labels](evo::popup_t *popup) {
                                            popup->bind_dropdown(setting->name, &setting->value.int_val, labels);
                                        });
                                        dirty_checks.emplace_back([before, setting]() {
                                            if (before != setting->value.int_val)
                                                cradle::config::ConfigManager::mark_dirty();
                                        });
                                    };

                                    bind_checkbox("enable esp", general_bindings);
                                    static const std::vector<std::string> kBoxModes = {"2d box", "3d box"};
                                    bind_dropdown("esp box mode", kBoxModes, general_bindings);
                                    bind_checkbox("names", general_bindings);
                                    bind_checkbox("distance", general_bindings);
                                    bind_checkbox("health bar", general_bindings);
                                    bind_checkbox("head circle", general_bindings);
                                    bind_checkbox("skeleton", general_bindings);
                                    bind_checkbox("wall check", general_bindings);
                                    bind_checkbox("team check", general_bindings);
                                    bind_slider_int("esp max renders", general_bindings, " players");
                                    bind_slider_float("max distance", general_bindings, " studs");

                                    bind_slider_float("refresh ms", performance_bindings, " ms");
                                    bind_slider_int("refresh limit", performance_bindings, " /frame");
                                    bind_slider_float("skeleton refresh ms", performance_bindings, " ms");
                                    bind_slider_int("skeleton max refresh per frame", performance_bindings, " /frame");
                                    bind_slider_int("skeleton max draws per frame", performance_bindings, " draws");
                                    bind_slider_float("skeleton max distance", performance_bindings, " studs");
                                    bind_checkbox("skeleton visible only", performance_bindings);
                                    bind_checkbox("wallcheck debug overlay", performance_bindings);

                                    if (!general_bindings.empty())
                                    {
                                        auto popup = new evo::popup_t(kGeneralPopupId, "general esp");
                                        for (auto &bind : general_bindings)
                                            bind(popup);
                                        child->obj(popup);
                                    }
                                    else
                                    {
                                        child->make_text("general esp controls unavailable");
                                    }

                                    if (!performance_bindings.empty())
                                    {
                                        auto popup = new evo::popup_t(kPerformancePopupId, "performance + skeleton");
                                        for (auto &bind : performance_bindings)
                                            bind(popup);
                                        child->obj(popup);
                                    }
                                    else
                                    {
                                        child->make_text("performance controls unavailable");
                                    }

                                    for (auto &check : dirty_checks)
                                        check();

                                    child->make_text("Color pickers now live under Visual Colors → ESP");
                                }

                                void RenderVisualColorPanel(evo::child_t *child)
                                {
                                    auto &manager = cradle::modules::ModuleManager::get_instance();

                                    child->make_text("ESP color controls");
                                    if (auto *esp_module = manager.find_module("esp"))
                                    {
                                        bool any_esp_colors = false;
                                        for (auto &setting : esp_module->get_settings())
                                        {
                                            if (setting.type != cradle::modules::SettingType::COLOR)
                                                continue;
                                            any_esp_colors = true;
                                            RenderColorSettingControl(child, setting);
                                        }
                                        if (!any_esp_colors)
                                            child->make_text("No ESP color pickers are available right now.");
                                    }
                                    else
                                    {
                                        child->make_text("ESP module unavailable");
                                    }

                                    child->make_text("Aimbot color controls");
                                    if (auto *aimbot_module = manager.find_module("aimbot"))
                                    {
                                        bool any_aimbot_colors = false;
                                        for (auto &setting : aimbot_module->get_settings())
                                        {
                                            if (setting.type != cradle::modules::SettingType::COLOR)
                                                continue;
                                            any_aimbot_colors = true;
                                            RenderColorSettingControl(child, setting);
                                        }
                                        if (!any_aimbot_colors)
                                            child->make_text("No aimbot color pickers are available right now.");
                                    }
                                    else
                                    {
                                        child->make_text("Aimbot module unavailable");
                                    }
                                }

                                void RenderCheatIndicator()
                                {
                                    ImGuiViewport *viewport = ImGui::GetMainViewport();
                                    if (!viewport)
                                        return;
                                    ImDrawList *indicator_draw_list = ImGui::GetForegroundDrawList();
                                    if (!indicator_draw_list)
                                        return;

                                    auto &manager = cradle::modules::ModuleManager::get_instance();
                                    const float padding = 8.0f;
                                    const float spacing = 4.0f;
                                    float max_text_width = 0.0f;
                                    std::array<std::string, kCheatIndicatorCount> labels{};
                                    std::array<ImU32, kCheatIndicatorCount> colors{};

                                    for (std::size_t i = 0; i < kCheatIndicatorCount; ++i)
                                    {
                                        auto *module = manager.find_module(kCheatIndicatorEntries[i].module_name);
                                        bool available = module != nullptr;
                                        bool active = available && module->is_enabled();
                                        const char *state = available ? (active ? "ON" : "OFF") : "N/A";
                                        labels[i] = std::string(kCheatIndicatorEntries[i].label) + ": " + state;
                                        ImVec2 size = ImGui::CalcTextSize(labels[i].c_str());
                                        max_text_width = std::max(max_text_width, size.x);

                                        if (!available)
                                            colors[i] = IM_COL32(180, 180, 180, 255);
                                        else if (active)
                                            colors[i] = IM_COL32(120, 230, 140, 255);
                                        else
                                            colors[i] = IM_COL32(235, 95, 95, 255);
                                    }

                                    float line_height = ImGui::GetFontSize();
                                    float box_width = max_text_width + padding * 2.0f;
                                    float box_height = kCheatIndicatorCount * line_height + (kCheatIndicatorCount - 1) * spacing + padding * 2.0f;

                                    ImVec2 top_right(
                                        viewport->Pos.x + viewport->Size.x - box_width - 18.0f,
                                        viewport->Pos.y + 18.0f);
                                    ImVec2 bottom_right(top_right.x + box_width, top_right.y + box_height);

                                    indicator_draw_list->AddRectFilled(top_right, bottom_right, IM_COL32(12, 12, 12, 205), 6.0f);
                                    indicator_draw_list->AddRect(top_right, bottom_right, IM_COL32(255, 255, 255, 35), 6.0f, 0, 1.0f);

                                    float y = top_right.y + padding;
                                    for (std::size_t i = 0; i < kCheatIndicatorCount; ++i)
                                    {
                                        ImVec2 pos(top_right.x + padding, y);
                                        indicator_draw_list->AddText(pos, colors[i], labels[i].c_str());
                                        y += line_height + spacing;
                                    }
                                }

                                static constexpr float kLoadingOverlayWidth = 640.0f;
                                static constexpr float kLoadingOverlayHeight = 360.0f;
                                static bool g_loader_capture_requested = false;
                                static ImRect g_loader_bounds(ImVec2(0.0f, 0.0f), ImVec2(0.0f, 0.0f));

                                static constexpr int kLoadingStageCount = 4;
                                static constexpr std::size_t kLoaderLogCapacity = 64;
                                static constexpr std::array<double, kLoadingStageCount> kStageProgressDurations = {0.0, 6.0, 3.5, 1.0};
                                static std::atomic<double> g_loader_stage_change_time{0.0};

                                static std::mutex g_loader_log_mutex;
                                static std::deque<std::string> g_loader_log_lines;

                                static const char *LoadingStageDisplayText(Overlay::LoadingStage stage)
                                {
                                    switch (stage)
                                    {
                                    case Overlay::LoadingStage::WaitingForRoblox:
                                        return "waiting";
                                    case Overlay::LoadingStage::Attaching:
                                        return "attaching";
                                    case Overlay::LoadingStage::Initializing:
                                        return "initializing";
                                    case Overlay::LoadingStage::Ready:
                                        return "ready";
                                    case Overlay::LoadingStage::Failed:
                                        return "failed";
                                    default:
                                        return "unknown";
                                    }
                                }

                                static int LoadingStageToIndex(Overlay::LoadingStage stage)
                                {
                                    switch (stage)
                                    {
                                    case Overlay::LoadingStage::WaitingForRoblox:
                                        return 0;
                                    case Overlay::LoadingStage::Attaching:
                                        return 1;
                                    case Overlay::LoadingStage::Initializing:
                                        return 2;
                                    case Overlay::LoadingStage::Ready:
                                        return 3;
                                    case Overlay::LoadingStage::Failed:
                                        return 4;
                                    default:
                                        return 0;
                                    }
                                }

                                static double GetNowSeconds()
                                {
                                    using clock = std::chrono::steady_clock;
                                    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
                                }

                                static float CalculateLoaderProgress(Overlay::LoadingStage stage)
                                {
                                    static float s_displayed_progress = 0.0f;
                                    static double s_last_update_time = 0.0;

                                    if constexpr (kLoadingStageCount <= 1)
                                        return 1.0f;

                                    const double now = GetNowSeconds();
                                    const int stage_index = LoadingStageToIndex(stage);
                                    double base = static_cast<double>(stage_index);
                                    if (stage != Overlay::LoadingStage::WaitingForRoblox &&
                                        stage_index >= 0 &&
                                        stage_index < (kLoadingStageCount - 1))
                                    {
                                        const double stage_start = g_loader_stage_change_time.load(std::memory_order_relaxed);
                                        const double elapsed = std::max(0.0, now - stage_start);
                                        const std::size_t duration_index = static_cast<std::size_t>(
                                            std::min(stage_index, static_cast<int>(kStageProgressDurations.size() - 1)));
                                        double duration = kStageProgressDurations[duration_index];
                                        if (duration <= 0.0)
                                            duration = 1.0;
                                        const double stage_fraction = std::min(elapsed / duration, 0.95);
                                        base += stage_fraction;
                                    }

                                    double normalized = base / static_cast<double>(kLoadingStageCount - 1);
                                    float target = static_cast<float>(std::clamp(normalized, 0.0, 1.0));

                                    const double delta_seconds = std::max(0.0, now - s_last_update_time);
                                    float max_step = static_cast<float>(delta_seconds * 1.25);
                                    if (max_step < 0.02f)
                                        max_step = 0.02f;

                                    const float delta = target - s_displayed_progress;
                                    if (std::fabs(delta) <= max_step)
                                        s_displayed_progress = target;
                                    else
                                        s_displayed_progress += (delta > 0.0f ? max_step : -max_step);

                                    s_displayed_progress = std::clamp(s_displayed_progress, 0.0f, 1.0f);
                                    s_last_update_time = now;
                                    return s_displayed_progress;
                                }

                                static void AppendLoaderLogLine(const std::string &line)
                                {
                                    std::lock_guard<std::mutex> lock(g_loader_log_mutex);
                                    g_loader_log_lines.push_back(line);
                                    while (g_loader_log_lines.size() > kLoaderLogCapacity)
                                        g_loader_log_lines.pop_front();
                                }

                                class BootstrapConsoleSink : public spdlog::sinks::base_sink<std::mutex>
                                {
                                protected:
                                    void sink_it_(const spdlog::details::log_msg &msg) override
                                    {
                                        spdlog::memory_buf_t buf;
                                        formatter_->format(msg, buf);
                                        AppendLoaderLogLine(fmt::to_string(buf));
                                    }

                                    void flush_() override {}
                                };

                                static void InstallBootstrapConsoleSink()
                                {
                                    static bool installed = false;
                                    if (installed)
                                        return;

                                    auto sink = std::make_shared<BootstrapConsoleSink>();
                                    sink->set_pattern("[%H:%M:%S] %v");
                                    auto logger = std::make_shared<spdlog::logger>("bootstrap", sink);
                                    logger->set_pattern("[%H:%M:%S] %v");
                                    logger->set_level(spdlog::level::info);
                                    spdlog::set_default_logger(logger);
                                    installed = true;
                                }

                                [[maybe_unused]] static std::vector<std::string> GetLoaderLogSnapshot()
                                {
                                    std::lock_guard<std::mutex> lock(g_loader_log_mutex);
                                    return std::vector<std::string>(g_loader_log_lines.begin(), g_loader_log_lines.end());
                                }

                                [[maybe_unused]] static std::filesystem::path GetLoaderLogFilePath()
                                {
                                    std::filesystem::path base = std::filesystem::current_path();
                                    base /= "logs";
                                    return base / "bootstrap_console.txt";
                                }

                                [[maybe_unused]] static bool ExportLoaderLogs()
                                {
                                    auto lines = GetLoaderLogSnapshot();
                                    auto path = GetLoaderLogFilePath();
                                    std::error_code ec;
                                    if (!path.parent_path().empty())
                                        std::filesystem::create_directories(path.parent_path(), ec);

                                    std::ofstream file(path, std::ios::trunc);
                                    if (!file.is_open())
                                    {
                                        AppendLoaderLogLine("failed to export bootstrap log (file error)");
                                        return false;
                                    }

                                    file << "bootstrap console log" << std::endl
                                         << "----------------------" << std::endl;
                                    for (const auto &line : lines)
                                        file << line << std::endl;
                                    file.close();

                                    AppendLoaderLogLine(std::string("exported bootstrap log to ") + path.string());
                                    return true;
                                }

                                static void EnsureConsoleHidden()
                                {
                                    static bool hidden = false;
                                    if (hidden)
                                        return;

                                    HWND console = GetConsoleWindow();
                                    if (!console)
                                        return;

                                    ShowWindow(console, SW_HIDE);
                                    hidden = true;
                                }

                                static std::string CollapseWhitespace(const std::string &text)
                                {
                                    if (text.empty())
                                        return text;

                                    std::string result;
                                    result.reserve(text.size());
                                    bool previous_space = true;
                                    for (char ch : text)
                                    {
                                        char c = ch;
                                        if (c == '\n' || c == '\r' || c == '\t')
                                            c = ' ';

                                        if (c == ' ')
                                        {
                                            if (previous_space)
                                                continue;
                                            previous_space = true;
                                        }
                                        else
                                        {
                                            previous_space = false;
                                        }

                                        result.push_back(c);
                                    }

                                    while (!result.empty() && result.back() == ' ')
                                        result.pop_back();

                                    return result;
                                }

                                [[maybe_unused]] static std::vector<std::string> WrapText(const std::string &text, std::size_t max_chars)
                                {
                                    std::vector<std::string> lines;
                                    if (text.empty())
                                    {
                                        lines.emplace_back();
                                        return lines;
                                    }

                                    std::istringstream stream(text);
                                    std::string word;
                                    std::string current;
                                    while (stream >> word)
                                    {
                                        if (current.empty())
                                        {
                                            current = word;
                                            continue;
                                        }

                                        if (current.size() + 1 + word.size() <= max_chars)
                                        {
                                            current.push_back(' ');
                                            current += word;
                                        }
                                        else
                                        {
                                            lines.push_back(current);
                                            current = word;
                                        }
                                    }

                                    if (!current.empty())
                                        lines.push_back(current);

                                    if (lines.empty())
                                        lines.emplace_back();

                                    return lines;
                                }

                                static std::string BuildStatusAndProgressText(const std::string &status_line,
                                                             const char *stage_text,
                                                             const char *progress_label)
                                {
                                    std::string text;
                                    text.reserve(256);

                                    text += "status · ";
                                    text += stage_text;
                                    std::string collapsed = CollapseWhitespace(status_line);
                                    if (!collapsed.empty())
                                    {
                                        text += " — ";
                                        text += collapsed;
                                    }
                                    text += "\nprogress · ";
                                    text += progress_label;
                                    return text;
                                }

                                static void RenderLoadingWindowContent(evo::window_t *window,
                                                                     Overlay::LoadingStage stage,
                                                                     const std::string &raw_message)
                                {
                                    constexpr int kLoaderTabIndex = 0;

                                    const char *stage_text = "loading";
                                    switch (stage)
                                    {
                                    case Overlay::LoadingStage::WaitingForRoblox:
                                        stage_text = "waiting for roblox";
                                        break;
                                    case Overlay::LoadingStage::Attaching:
                                        stage_text = "roblox detected";
                                        break;
                                    case Overlay::LoadingStage::Initializing:
                                        stage_text = "initializing modules";
                                        break;
                                    case Overlay::LoadingStage::Ready:
                                        stage_text = "ready";
                                        break;
                                    case Overlay::LoadingStage::Failed:
                                        stage_text = "shutting down";
                                        break;
                                    }

                                    std::string collapsed_message = CollapseWhitespace(raw_message);
                                    if (collapsed_message.empty())
                                        collapsed_message = "Preparing environment...";

                                    float progress_fraction = CalculateLoaderProgress(stage);
                                    int progress_pct = static_cast<int>(std::round(progress_fraction * 100.0f));
                                    progress_pct = std::clamp(progress_pct, 0, 100);
                                    char progress_label[32];
                                    std::snprintf(progress_label, sizeof(progress_label), "%d%%", progress_pct);

                                    auto console_child = new evo::child_t("bootstrap console", 0, evo::child_pos_t::_top_left, evo::child_size_t::_full, kLoaderTabIndex);
                                    console_child->full_child();
                                    {
                                        window->make_child(console_child);
                                        std::string console_block = BuildStatusAndProgressText(collapsed_message, stage_text, progress_label);
                                        console_child->make_text(console_block, console_child->get_content_height());
                                    }
                                    delete console_child;
                                }

                                static void RenderLoadingOverlay(Overlay::LoadingStage stage, const std::string &message)
                                {
                                    ImGuiViewport *viewport = ImGui::GetMainViewport();
                                    if (!viewport)
                                        return;

                                    g_loader_capture_requested = false;
                                    EnsureConsoleHidden();

                                    const evo::vec2_t window_size(kLoadingOverlayWidth, kLoadingOverlayHeight);
                                    static evo::vec2_t loader_pos(0.0f, 0.0f);
                                    static evo::vec2_t last_pos_recorded(0.0f, 0.0f);
                                    static ImVec2 last_viewport_size(0.0f, 0.0f);
                                    static bool seed_position = false;
                                    static bool user_moved = false;
                                    static Overlay::LoadingStage last_stage = Overlay::LoadingStage::WaitingForRoblox;

                                    ImVec2 viewport_center = viewport->Pos + viewport->Size * 0.5f;
                                    evo::vec2_t centered_pos(viewport_center.x - window_size.x * 0.5f,
                                                             viewport_center.y - window_size.y * 0.5f);

                                    bool viewport_changed = (last_viewport_size.x != viewport->Size.x || last_viewport_size.y != viewport->Size.y);
                                    if (!seed_position || (!user_moved && viewport_changed))
                                    {
                                        loader_pos = centered_pos;
                                        last_pos_recorded = loader_pos;
                                        seed_position = true;
                                    }

                                    if (stage == Overlay::LoadingStage::WaitingForRoblox && last_stage == Overlay::LoadingStage::Ready)
                                    {
                                        loader_pos = centered_pos;
                                        last_pos_recorded = loader_pos;
                                        user_moved = false;
                                    }

                                    last_viewport_size = viewport->Size;
                                    last_stage = stage;

                                    float min_x = viewport->Pos.x + 12.0f;
                                    float min_y = viewport->Pos.y + 12.0f;
                                    float max_x = viewport->Pos.x + viewport->Size.x - window_size.x - 12.0f;
                                    float max_y = viewport->Pos.y + viewport->Size.y - window_size.y - 12.0f;
                                    if (max_x < min_x)
                                        max_x = min_x;
                                    if (max_y < min_y)
                                        max_y = min_y;

                                    loader_pos.x = std::clamp(loader_pos.x, min_x, max_x);
                                    loader_pos.y = std::clamp(loader_pos.y, min_y, max_y);

                                    static int loader_tab = 0;
                                    static const std::vector<std::string> loader_tabs = {};

                                    const auto prev_orx = evo::theme::orx;
                                    const auto prev_ory = evo::theme::ory;
                                    const auto prev_rx = evo::theme::rx;
                                    const auto prev_ry = evo::theme::ry;
                                    const auto prev_menu_size = evo::theme::menu_size;
                                    bool prev_menu_resizable = evo::theme::menu_resizable;

                                    evo::theme::orx = static_cast<int>(window_size.x);
                                    evo::theme::ory = static_cast<int>(window_size.y);
                                    evo::theme::rx = static_cast<int>(window_size.x);
                                    evo::theme::ry = static_cast<int>(window_size.y);
                                    evo::theme::menu_size = window_size;
                                    evo::theme::menu_resizable = false;

                                    g_loader_bounds = ImRect(ImVec2(loader_pos.x, loader_pos.y),
                                                             ImVec2(loader_pos.x + window_size.x, loader_pos.y + window_size.y));

                                    auto loader_window = new evo::window_t("bootstrap", &loader_pos, window_size, loader_tabs, &loader_tab);
                                    {
                                        RenderLoadingWindowContent(loader_window, stage, message);
                                    }
                                    delete loader_window;

                                    ImGuiIO &io = ImGui::GetIO();
                                    if (io.MousePos.x >= g_loader_bounds.Min.x && io.MousePos.x <= g_loader_bounds.Max.x &&
                                        io.MousePos.y >= g_loader_bounds.Min.y && io.MousePos.y <= g_loader_bounds.Max.y)
                                    {
                                        g_loader_capture_requested = true;
                                    }

                                    if (std::fabs(loader_pos.x - last_pos_recorded.x) > 0.5f || std::fabs(loader_pos.y - last_pos_recorded.y) > 0.5f)
                                    {
                                        user_moved = true;
                                        last_pos_recorded = loader_pos;
                                    }

                                    evo::theme::orx = prev_orx;
                                    evo::theme::ory = prev_ory;
                                    evo::theme::rx = prev_rx;
                                    evo::theme::ry = prev_ry;
                                    evo::theme::menu_size = prev_menu_size;
                                    evo::theme::menu_resizable = prev_menu_resizable;
                                }

                                void RenderProfilingPanel(evo::child_t *child, cradle::modules::Module *module)
                                {
                                    child->make_text("Profiling overlays");
                                    for (auto &setting : module->get_settings())
                                    {
                                        if (setting.type != cradle::modules::SettingType::BOOL)
                                            continue;
                                        bool before = setting.value.bool_val;
                                        child->make_checkbox(setting.name, &setting.value.bool_val);
                                        if (before != setting.value.bool_val)
                                            cradle::config::ConfigManager::mark_dirty();
                                    }

                                    auto &manager = cradle::modules::ModuleManager::get_instance();
                                    auto *esp_module = manager.find_module("esp");
                                    if (esp_module)
                                    {
                                        bool rendered_controls = false;
                                        auto esp_refresh_ms = esp_module->get_setting("esp refresh ms");
                                        auto esp_refresh_limit = esp_module->get_setting("esp refresh limit");
                                        auto esp_wallcheck_debug = esp_module->get_setting("wallcheck debug overlay");

                                        if (esp_refresh_ms)
                                        {
                                            child->make_text("ESP refresh controls");
                                            float before = esp_refresh_ms->value.float_val;
                                            child->make_slider_float(esp_refresh_ms->name, &esp_refresh_ms->value.float_val,
                                                                     esp_refresh_ms->range.float_range.min,
                                                                     esp_refresh_ms->range.float_range.max, "");
                                            if (before != esp_refresh_ms->value.float_val)
                                                cradle::config::ConfigManager::mark_dirty();
                                            rendered_controls = true;
                                        }

                                        if (esp_refresh_limit)
                                        {
                                            if (!rendered_controls)
                                                child->make_text("ESP refresh controls");
                                            int before = esp_refresh_limit->value.int_val;
                                            child->make_slider_int(esp_refresh_limit->name, &esp_refresh_limit->value.int_val,
                                                                   esp_refresh_limit->range.int_range.min,
                                                                   esp_refresh_limit->range.int_range.max, "");
                                            if (before != esp_refresh_limit->value.int_val)
                                                cradle::config::ConfigManager::mark_dirty();
                                            rendered_controls = true;
                                        }

                                        if (esp_wallcheck_debug)
                                        {
                                            if (!rendered_controls)
                                                child->make_text("ESP diagnostics");
                                            bool before = esp_wallcheck_debug->value.bool_val;
                                            child->make_checkbox(esp_wallcheck_debug->name, &esp_wallcheck_debug->value.bool_val);
                                            if (before != esp_wallcheck_debug->value.bool_val)
                                                cradle::config::ConfigManager::mark_dirty();
                                            rendered_controls = true;
                                        }

                                        if (!rendered_controls)
                                        {
                                            child->make_text("ESP refresh controls unavailable.");
                                        }
                                    }
                                    else
                                    {
                                        child->make_text("ESP module unavailable; refresh controls hidden.");
                                    }

                                    child->make_text("Current metrics");
                                    auto snapshot = cradle::profiling::get_snapshot();

                                    char buffer[192];
                                    std::snprintf(buffer, sizeof(buffer),
                                                  "ESP cache · refresh %.2f ms · attempts %.1f · success %.1f",
                                                  snapshot.esp_refresh_ms,
                                                  snapshot.esp_attempts,
                                                  snapshot.esp_success);
                                    child->make_text(buffer);

                                    std::snprintf(buffer, sizeof(buffer),
                                                  "Player cache · refresh %.2f ms · players %zu",
                                                  snapshot.playercache_ms,
                                                  snapshot.playercache_players);
                                    child->make_text(buffer);

                                    std::snprintf(buffer, sizeof(buffer),
                                                  "Draw build · %.2f ms · cmd lists %d (%d cmds) · vtx %d · idx %d",
                                                  snapshot.draw_build_ms,
                                                  snapshot.draw_cmd_lists,
                                                  snapshot.draw_total_cmds,
                                                  snapshot.draw_vertices,
                                                  snapshot.draw_indices);
                                    child->make_text(buffer);
                                }

                                void RenderConfigTab(evo::window_t *window)
                                {
                                    int tab_index = static_cast<int>(EvoTab::Config);
                                    auto info_child = new evo::child_t("config_info", 0, evo::child_pos_t::_top_left, evo::child_size_t::_full, tab_index);
                                    {
                                        window->make_child(info_child);
                                        info_child->make_text("Configuration overview");
                                        info_child->make_text("The default preset keeps every feature disabled until you opt in.");
                                        info_child->make_text("Use the actions on the right to save, load, or reset your config.");
                                    }
                                    delete info_child;

                                    auto actions_child = new evo::child_t("config_actions", 1, evo::child_pos_t::_top_right, evo::child_size_t::_full, tab_index);
                                    {
                                        window->make_child(actions_child);
                                        actions_child->make_text("Active file: config/mossad_config.cfg");
                                        actions_child->make_button("save config", []() {
                                            auto &manager = cradle::modules::ModuleManager::get_instance();
                                            cradle::config::ConfigManager::save_now(manager);
                                        });
                                        actions_child->make_button("load config", []() {
                                            auto &manager = cradle::modules::ModuleManager::get_instance();
                                            cradle::config::ConfigManager::load_now(manager);
                                        });
                                        actions_child->make_button("reset to default (all disabled)", []() {
                                            auto &manager = cradle::modules::ModuleManager::get_instance();
                                            cradle::config::ConfigManager::reset_to_defaults(manager);
                                            cradle::config::ConfigManager::tick(manager, true);
                                        });
                                    }
                                    delete actions_child;
                                }

                                void RenderWalkspeedMemorySection(evo::child_t *child)
                                {
                                    auto &state = g_movement_override_state;
                                    child->make_text("Walkspeed overrides");

                                    auto local_player = cradle::engine::PlayerCache::get_local_player();
                                    EnsureMovementDefaults(state, local_player);
                                    bool humanoid_ready = local_player.humanoid.is_valid();
                                    state.humanoid_ready = humanoid_ready;
                                    child->make_text(humanoid_ready ? "humanoid ready" : "humanoid missing");

                                    child->make_slider_float("walkspeed##memwrite", &state.custom_walkspeed, 4.0f, 250.0f, "");
                                    child->make_checkbox("lock walkspeed", &state.lock_walkspeed);

                                    child->make_button("apply walkspeed", [&]() {
                                        auto player = cradle::engine::PlayerCache::get_local_player();
                                        if (!player.humanoid.is_valid())
                                        {
                                            state.last_walk_apply_success = false;
                                            state.enforce_walk_after_apply = false;
                                            return;
                                        }

                                        state.capture_next_defaults = true;
                                        EnsureMovementDefaults(state, player);
                                        bool applied = ApplyWalkspeedOverride(state, player, state.custom_walkspeed, true);
                                        state.last_walk_apply_success = applied;
                                        state.enforce_walk_after_apply = true;
                                        state.last_walk_enforce_time = ImGui::GetTime();
                                    });

                                    if (state.defaults_initialized)
                                    {
                                        child->make_button("reset walkspeed", [&]() {
                                            auto player = cradle::engine::PlayerCache::get_local_player();
                                            if (!player.humanoid.is_valid())
                                            {
                                                state.enforce_walk_after_apply = false;
                                                state.last_walk_apply_success = false;
                                                return;
                                            }

                                            state.custom_walkspeed = state.default_walkspeed;
                                            bool applied = ApplyWalkspeedOverride(state, player, state.default_walkspeed, true);
                                            state.last_walk_apply_success = applied;
                                            state.enforce_walk_after_apply = false;
                                        });

                                        char defaults_buf[64];
                                        std::snprintf(defaults_buf, sizeof(defaults_buf), "default walk %.1f", state.default_walkspeed);
                                        child->make_text(defaults_buf);
                                    }

                                    if (state.last_walk_apply_success)
                                    {
                                        char buffer[96];
                                        std::snprintf(buffer, sizeof(buffer), "walk %.1f applied %.1fs ago",
                                                      state.last_walk_apply_value,
                                                      static_cast<float>(ImGui::GetTime()) - state.last_walk_apply_time);
                                        child->make_text(buffer);
                                    }
                                    else if (!humanoid_ready)
                                    {
                                        child->make_text("spawn in-game to edit walkspeed");
                                    }
                                }

                                void RenderJumpHeightMemorySection(evo::child_t *child)
                                {
                                    auto &state = g_movement_override_state;
                                    child->make_text("Jump height overrides");

                                    auto local_player = cradle::engine::PlayerCache::get_local_player();
                                    EnsureMovementDefaults(state, local_player);
                                    bool humanoid_ready = local_player.humanoid.is_valid();
                                    state.humanoid_ready = humanoid_ready;
                                    child->make_text(humanoid_ready ? "humanoid ready" : "humanoid missing");

                                    child->make_slider_float("jump height##memwrite", &state.custom_jumpheight, 4.0f, 200.0f, "");
                                    child->make_checkbox("lock jump height", &state.lock_jumpheight);

                                    child->make_button("apply jump height", [&]() {
                                        auto player = cradle::engine::PlayerCache::get_local_player();
                                        if (!player.humanoid.is_valid())
                                        {
                                            state.last_jump_apply_success = false;
                                            state.enforce_jump_after_apply = false;
                                            return;
                                        }

                                        state.capture_next_defaults = true;
                                        EnsureMovementDefaults(state, player);
                                        bool applied = ApplyJumpHeightOverride(state, player, state.custom_jumpheight, true);
                                        state.last_jump_apply_success = applied;
                                        state.enforce_jump_after_apply = true;
                                        state.last_jump_enforce_time = ImGui::GetTime();
                                    });

                                    if (state.defaults_initialized)
                                    {
                                        child->make_button("reset jump height", [&]() {
                                            auto player = cradle::engine::PlayerCache::get_local_player();
                                            if (!player.humanoid.is_valid())
                                            {
                                                state.enforce_jump_after_apply = false;
                                                state.last_jump_apply_success = false;
                                                return;
                                            }

                                            state.custom_jumpheight = state.default_jumpheight;
                                            bool applied = ApplyJumpHeightOverride(state, player, state.default_jumpheight, true);
                                            state.last_jump_apply_success = applied;
                                            state.enforce_jump_after_apply = false;
                                        });

                                        char defaults_buf[64];
                                        std::snprintf(defaults_buf, sizeof(defaults_buf), "default jump %.1f", state.default_jumpheight);
                                        child->make_text(defaults_buf);
                                    }

                                    if (state.last_jump_apply_success)
                                    {
                                        char buffer[96];
                                        std::snprintf(buffer, sizeof(buffer), "jump %.1f applied %.1fs ago",
                                                      state.last_jump_apply_value,
                                                      static_cast<float>(ImGui::GetTime()) - state.last_jump_apply_time);
                                        child->make_text(buffer);
                                    }
                                    else if (!humanoid_ready)
                                    {
                                        child->make_text("spawn in-game to edit jump height");
                                    }
                                }

                                void RenderGravityMemorySection(evo::child_t *child)
                                {
                                    auto &gravity_state = g_gravity_override_state;
                                    child->make_text("Gravity overrides");

                                    child->make_slider_float("gravity##memwrite", &gravity_state.custom_gravity, 0.0f, 500.0f, "");
                                    child->make_checkbox("lock gravity", &gravity_state.lock_gravity);
                                    child->make_text(gravity_state.workspace_ready ? "workspace ready" : "workspace missing");

                                    child->make_button("apply gravity", [&]() {
                                        auto dm_instance = cradle::engine::DataModel::get_instance();
                                        auto workspace = dm_instance.get_workspace();

                                        if (!TryWriteWorkspaceGravity(workspace, gravity_state.custom_gravity))
                                        {
                                            gravity_state.last_apply_success = false;
                                            gravity_state.enforce_after_apply = false;
                                            return;
                                        }

                                        gravity_state.last_apply_success = true;
                                        gravity_state.last_apply_value = gravity_state.custom_gravity;
                                        gravity_state.last_apply_time = static_cast<float>(ImGui::GetTime());
                                        gravity_state.last_enforce_time = ImGui::GetTime();
                                        gravity_state.enforce_after_apply = true;
                                    });

                                    if (gravity_state.defaults_initialized)
                                    {
                                        child->make_button("reset gravity", [&]() {
                                            auto dm_instance = cradle::engine::DataModel::get_instance();
                                            auto workspace = dm_instance.get_workspace();

                                            if (!TryWriteWorkspaceGravity(workspace, gravity_state.default_gravity))
                                            {
                                                gravity_state.last_apply_success = false;
                                                gravity_state.enforce_after_apply = false;
                                                return;
                                            }

                                            gravity_state.custom_gravity = gravity_state.default_gravity;
                                            gravity_state.last_apply_success = true;
                                            gravity_state.last_apply_value = gravity_state.default_gravity;
                                            gravity_state.last_apply_time = static_cast<float>(ImGui::GetTime());
                                            gravity_state.last_enforce_time = ImGui::GetTime();
                                            gravity_state.enforce_after_apply = false;
                                        });

                                        char gravity_defaults[96];
                                        std::snprintf(gravity_defaults, sizeof(gravity_defaults), "default gravity %.1f",
                                                      gravity_state.default_gravity);
                                        child->make_text(gravity_defaults);
                                    }

                                    if (gravity_state.last_apply_success)
                                    {
                                        char buffer[128];
                                        std::snprintf(buffer, sizeof(buffer), "gravity %.1f applied %.1fs ago",
                                                      gravity_state.last_apply_value,
                                                      static_cast<float>(ImGui::GetTime()) - gravity_state.last_apply_time);
                                        child->make_text(buffer);
                                    }
                                    else if (!gravity_state.workspace_ready)
                                    {
                                        child->make_text("join a game to edit gravity");
                                    }
                                }

                                void RenderLightingColorPicker(evo::child_t *child,
                                                              const char *label,
                                                              cradle::engine::vector3 &value,
                                                              float &hue_cache,
                                                              LightingOverrideState &state)
                                {
                                    cradle::engine::vector3 before = value;
                                    evo::col_t color = Color3ToEvoColor(value);
                                    if (hue_cache <= 0.0f)
                                        hue_cache = color.hue();
                                    child->make_text(label);
                                    child->make_colorpicker(&color, &hue_cache);
                                    value = ClampLightingColor(EvoColorToColor3(color));
                                    if (LightingVectorChanged(before, value))
                                        MarkLightingPendingApply(state);
                                }

                                void RenderLightingMemorySection(evo::child_t *child)
                                {
                                    auto &state = g_lighting_override_state;
                                    child->make_text("Lighting reference overrides");
                                    child->make_text("Based on lr/lighting_reference.txt offsets (Ambient, OutdoorAmbient, Fog, Time).");

                                    auto dm_instance = cradle::engine::DataModel::get_instance();
                                    auto lighting = dm_instance.get_lighting();
                                    auto render_view = dm_instance.get_render_view();
                                    state.lighting_ready = lighting.is_valid();
                                    state.render_view_ready = render_view.is_valid();
                                    child->make_text(state.lighting_ready ? "lighting ready" : "lighting missing");
                                    child->make_text(state.render_view_ready ? "render view ready" : "render view missing");

                                    if (!state.lighting_ready)
                                    {
                                        state.defaults_initialized = false;
                                        state.last_lighting_address = 0;
                                        child->make_text("join a game to edit lighting");
                                        return;
                                    }

                                    EnsureLightingDefaults(state, lighting);

                                    RenderLightingColorPicker(child, "ambient##lighting", state.ambient, state.ambient_hue, state);
                                    RenderLightingColorPicker(child, "color shift (top)##lighting", state.color_shift_top, state.color_shift_top_hue, state);
                                    RenderLightingColorPicker(child, "color shift (bottom)##lighting", state.color_shift_bottom, state.color_shift_bottom_hue, state);
                                    RenderLightingColorPicker(child, "fog color##lighting", state.fog_color, state.fog_color_hue, state);
                                    RenderLightingColorPicker(child, "outdoor ambient##lighting", state.outdoor_ambient, state.outdoor_ambient_hue, state);

                                    state.brightness = std::clamp(state.brightness, 0.0f, 10.0f);
                                    float prev_brightness = state.brightness;
                                    child->make_slider_float("brightness##lighting", &state.brightness, 0.0f, 10.0f, "");
                                    if (LightingFloatChanged(prev_brightness, state.brightness))
                                        MarkLightingPendingApply(state);

                                    float prev_exposure = state.exposure_compensation;
                                    child->make_slider_float("exposure compensation##lighting", &state.exposure_compensation,
                                                             kLightingExposureMin, kLightingExposureMax, "");
                                    if (LightingFloatChanged(prev_exposure, state.exposure_compensation))
                                        MarkLightingPendingApply(state);

                                    float prev_fog_start = state.fog_start;
                                    child->make_slider_float("fog start##lighting", &state.fog_start, 0.0f, kLightingMaxFogDistance, "");
                                    if (LightingFloatChanged(prev_fog_start, state.fog_start, 0.01f))
                                        MarkLightingPendingApply(state);

                                    float prev_fog_end = state.fog_end;
                                    child->make_slider_float("fog end##lighting", &state.fog_end, 0.0f, kLightingMaxFogDistance, "");
                                    if (LightingFloatChanged(prev_fog_end, state.fog_end, 0.01f))
                                        MarkLightingPendingApply(state);

                                    float prev_clock = state.clock_time;
                                    child->make_slider_float("clock time##lighting", &state.clock_time,
                                                             kLightingClockMin, kLightingClockMax, "");
                                    if (LightingFloatChanged(prev_clock, state.clock_time, 0.001f))
                                        MarkLightingPendingApply(state);

                                    float prev_lat = state.geographic_latitude;
                                    child->make_slider_float("geographic latitude##lighting", &state.geographic_latitude,
                                                             kLightingLatitudeMin, kLightingLatitudeMax, "");
                                    if (LightingFloatChanged(prev_lat, state.geographic_latitude, 0.001f))
                                        MarkLightingPendingApply(state);
                                    child->make_checkbox("lock lighting overrides", &state.lock_lighting);

                                    cradle::engine::vector3 live_ambient;
                                    cradle::engine::vector3 live_outdoor;
                                    float live_brightness = 0.0f;
                                    float live_fog_start = 0.0f;
                                    float live_fog_end = 0.0f;
                                    float live_exposure = 0.0f;
                                    float live_clock = 0.0f;
                                    float live_latitude = 0.0f;
                                    if (TryReadLightingColor(lighting, Offsets::Lighting::Ambient, live_ambient) &&
                                        TryReadLightingColor(lighting, Offsets::Lighting::OutdoorAmbient, live_outdoor) &&
                                        TryReadLightingBrightness(lighting, live_brightness) &&
                                        TryReadLightingFloat(lighting, Offsets::Lighting::FogStart, live_fog_start) &&
                                        TryReadLightingFloat(lighting, Offsets::Lighting::FogEnd, live_fog_end) &&
                                        TryReadLightingFloat(lighting, Offsets::Lighting::ExposureCompensation, live_exposure) &&
                                        TryReadLightingFloat(lighting, Offsets::Lighting::ClockTime, live_clock) &&
                                        TryReadLightingFloat(lighting, Offsets::Lighting::GeographicLatitude, live_latitude))
                                    {
                                        char live_buf[192];
                                        std::snprintf(live_buf, sizeof(live_buf),
                                                      "ambient %.3f/%.3f/%.3f · outdoor %.3f/%.3f/%.3f",
                                                      live_ambient.X, live_ambient.Y, live_ambient.Z,
                                                      live_outdoor.X, live_outdoor.Y, live_outdoor.Z);
                                        child->make_text(live_buf);
                                        std::snprintf(live_buf, sizeof(live_buf),
                                                      "brightness %.2f · exposure %.2f · fog %.1f-%.1f · clock %.2f · lat %.2f",
                                                      live_brightness, live_exposure, live_fog_start, live_fog_end, live_clock, live_latitude);
                                        child->make_text(live_buf);
                                    }

                                    child->make_button("apply lighting", [&, render_view]() {
                                        bool applied = ApplyLightingOverrides(state, lighting, render_view);
                                        state.enforce_after_apply = applied;
                                        if (applied)
                                        {
                                            state.last_enforce_time = ImGui::GetTime();
                                            state.pending_apply = false;
                                        }
                                    });

                                    if (state.defaults_initialized)
                                    {
                                        child->make_button("reset lighting", [&, render_view]() {
                                            state.ambient = state.default_ambient;
                                            state.color_shift_bottom = state.default_color_shift_bottom;
                                            state.color_shift_top = state.default_color_shift_top;
                                            state.fog_color = state.default_fog_color;
                                            state.outdoor_ambient = state.default_outdoor_ambient;
                                            state.brightness = state.default_brightness;
                                            state.fog_start = state.default_fog_start;
                                            state.fog_end = state.default_fog_end;
                                            state.exposure_compensation = state.default_exposure_compensation;
                                            state.clock_time = state.default_clock_time;
                                            state.geographic_latitude = state.default_geographic_latitude;
                                            bool applied = ApplyLightingOverrides(state, lighting, render_view);
                                            state.enforce_after_apply = applied;
                                            if (applied)
                                            {
                                                state.last_enforce_time = ImGui::GetTime();
                                                state.pending_apply = false;
                                            }
                                        });

                                        char defaults_buffer[256];
                                        std::snprintf(defaults_buffer, sizeof(defaults_buffer),
                                                      "defaults · ambient %.3f/%.3f/%.3f · outdoor %.3f/%.3f/%.3f",
                                                      state.default_ambient.X,
                                                      state.default_ambient.Y,
                                                      state.default_ambient.Z,
                                                      state.default_outdoor_ambient.X,
                                                      state.default_outdoor_ambient.Y,
                                                      state.default_outdoor_ambient.Z);
                                        child->make_text(defaults_buffer);

                                        std::snprintf(defaults_buffer, sizeof(defaults_buffer),
                                                      "brightness %.2f · exposure %.2f · fog %.1f-%.1f · clock %.2f · lat %.2f",
                                                      state.default_brightness,
                                                      state.default_exposure_compensation,
                                                      state.default_fog_start,
                                                      state.default_fog_end,
                                                      state.default_clock_time,
                                                      state.default_geographic_latitude);
                                        child->make_text(defaults_buffer);
                                    }

                                    if (state.last_apply_time > 0.0f)
                                    {
                                        if (state.last_apply_success)
                                        {
                                            char buffer[96];
                                            std::snprintf(buffer, sizeof(buffer), "last apply succeeded %.1fs ago",
                                                          static_cast<float>(ImGui::GetTime()) - state.last_apply_time);
                                            child->make_text(buffer);
                                        }
                                        else
                                        {
                                            child->make_text("last apply failed · ensure lighting is writable");
                                        }
                                    }

                                    child->make_text("colors clamp to 0-1; fog/time/latitude limits follow Roblox Lighting constraints");
                                }

                                void RenderMemoryPanel(evo::child_t *child, MemoryPanelSection section)
                                {
                                    child->make_text("MemoryWrite · risky");

                                    switch (section)
                                    {
                                    case MemoryPanelSection::Walkspeed:
                                        RenderWalkspeedMemorySection(child);
                                        break;
                                    case MemoryPanelSection::JumpHeight:
                                        RenderJumpHeightMemorySection(child);
                                        break;
                                    case MemoryPanelSection::Gravity:
                                        RenderGravityMemorySection(child);
                                        break;
                                    case MemoryPanelSection::Lighting:
                                        RenderLightingMemorySection(child);
                                        break;
                                    default:
                                        child->make_text("Select a memory override module on the left.");
                                        break;
                                    }
                                }
                            } // namespace

                            UINT Overlay::resize_width = 0;
                            UINT Overlay::resize_height = 0;
                            Overlay *Overlay::instance = nullptr;
                            bool Overlay::menu_visible = false;
                            std::atomic<bool> Overlay::runtime_ready{false};
                            std::atomic<Overlay::LoadingStage> Overlay::loading_stage{Overlay::LoadingStage::WaitingForRoblox};
                            std::mutex Overlay::loading_message_mutex;
                            std::string Overlay::loading_message = "Waiting for Roblox...";

                            void Overlay::set_loading_stage(LoadingStage stage, const std::string &message)
                            {
                                LoadingStage previous_stage = loading_stage.exchange(stage, std::memory_order_relaxed);
                                if (previous_stage != stage || g_loader_stage_change_time.load(std::memory_order_relaxed) == 0.0)
                                    g_loader_stage_change_time.store(GetNowSeconds(), std::memory_order_relaxed);
                                {
                                    std::lock_guard<std::mutex> lock(loading_message_mutex);
                                    loading_message = message;
                                }

                                std::string collapsed = CollapseWhitespace(message);
                                if (collapsed.empty())
                                    collapsed = "Updating...";
                                std::string log_entry = std::string("[") + LoadingStageDisplayText(stage) + "] " + collapsed;
                                AppendLoaderLogLine(log_entry);
                            }

                            Overlay::LoadingStage Overlay::get_loading_stage()
                            {
                                return loading_stage.load(std::memory_order_relaxed);
                            }

                            std::string Overlay::get_loading_message()
                            {
                                std::lock_guard<std::mutex> lock(loading_message_mutex);
                                return loading_message;
                            }

                            void Overlay::set_runtime_ready(bool ready)
                            {
                                runtime_ready.store(ready, std::memory_order_release);
                                if (ready && loading_stage.load(std::memory_order_relaxed) != LoadingStage::Ready)
                                    loading_stage.store(LoadingStage::Ready, std::memory_order_relaxed);
                            }

                            bool Overlay::is_runtime_ready()
                            {
                                return runtime_ready.load(std::memory_order_acquire);
                            }

                            bool Overlay::initialize()
                            {
                                if (instance)
                                    return true;

                                instance = this;
                                InstallBootstrapConsoleSink();
                                EnsureConsoleHidden();
                                g_loader_stage_change_time.store(GetNowSeconds(), std::memory_order_relaxed);
                                createWindow();
                                if (!overlayWindow)
                                {
                                    spdlog::error("failed to create overlay window");
                                    return false;
                                }

                                if (!setupDirectX())
                                {
                                    spdlog::error("failed to initialize dx11 context");
                                    cleanup();
                                    return false;
                                }

                                if (!setupImGui())
                                {
                                    spdlog::error("failed to initialize imgui context");
                                    cleanup();
                                    return false;
                                }

                                updateInputPassthrough(false);
                                return true;
                            }

                            void Overlay::createWindow()
                            {
                                WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
                                wc.style = CS_CLASSDC;
                                wc.lpfnWndProc = Overlay::windowProc;
                                wc.hInstance = GetModuleHandle(nullptr);
                                wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
                                wc.lpszClassName = kOverlayWindowClass;

                                RegisterClassExW(&wc);

                                int width = GetSystemMetrics(SM_CXSCREEN);
                                int height = GetSystemMetrics(SM_CYSCREEN);

                                overlayWindow = CreateWindowExW(
                                    WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
                                    kOverlayWindowClass,
                                    L"Cradle Overlay",
                                    WS_POPUP,
                                    0,
                                    0,
                                    width,
                                    height,
                                    nullptr,
                                    nullptr,
                                    wc.hInstance,
                                    nullptr);

                                if (!overlayWindow)
                                    return;

                                MARGINS margins = {-1};
                                DwmExtendFrameIntoClientArea(overlayWindow, &margins);
                                SetLayeredWindowAttributes(overlayWindow, 0, 255, LWA_ALPHA);
                                ShowWindow(overlayWindow, SW_SHOW);
                                UpdateWindow(overlayWindow);
                            }

                            bool Overlay::setupDirectX()
                            {
                                DXGI_SWAP_CHAIN_DESC desc = {};
                                desc.BufferCount = 2;
                                desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                                desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                                desc.OutputWindow = overlayWindow;
                                desc.SampleDesc.Count = 1;
                                desc.SampleDesc.Quality = 0;
                                desc.Windowed = TRUE;
                                desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
                                desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

                                static const D3D_FEATURE_LEVEL feature_levels[] = {
                                    D3D_FEATURE_LEVEL_11_0,
                                    D3D_FEATURE_LEVEL_10_0};

                                D3D_FEATURE_LEVEL selected_level;

                                UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
                    #ifdef _DEBUG
                                flags |= D3D11_CREATE_DEVICE_DEBUG;
                    #endif

                                HRESULT hr = D3D11CreateDeviceAndSwapChain(
                                    nullptr,
                                    D3D_DRIVER_TYPE_HARDWARE,
                                    nullptr,
                                    flags,
                                    feature_levels,
                                    ARRAYSIZE(feature_levels),
                                    D3D11_SDK_VERSION,
                                    &desc,
                                    &swapchain,
                                    &device,
                                    &selected_level,
                                    &context);

                                if (FAILED(hr))
                                    return false;

                                createRenderTarget();
                                return true;
                            }

                            bool Overlay::setupImGui()
                            {
                                IMGUI_CHECKVERSION();
                                ImGui::CreateContext();
                                ImGuiIO &io = ImGui::GetIO();
                                io.IniFilename = nullptr;
                                io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

                                ImGui::StyleColorsDark();

                                if (!ImGui_ImplWin32_Init(overlayWindow))
                                    return false;
                                if (!ImGui_ImplDX11_Init(device, context))
                                    return false;

                                evo::_render->initialize_font_system();
                                return true;
                            }

                            void Overlay::createRenderTarget()
                            {
                                ID3D11Texture2D *back_buffer = nullptr;
                                if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&back_buffer))))
                                    return;

                                device->CreateRenderTargetView(back_buffer, nullptr, &renderTargetView);
                                back_buffer->Release();
                            }

                            void Overlay::cleanupRenderTarget()
                            {
                                if (renderTargetView)
                                {
                                    renderTargetView->Release();
                                    renderTargetView = nullptr;
                                }
                            }

                            void Overlay::updateWindowPosition()
                            {
                                if (!overlayWindow)
                                    return;

                                auto update_visibility = [&](bool visible) {
                                    static bool overlay_visible = true;
                                    if (visible == overlay_visible)
                                        return;
                                    ShowWindow(overlayWindow, visible ? SW_SHOW : SW_HIDE);
                                    overlay_visible = visible;
                                };

                                using clock = std::chrono::steady_clock;
                                static HWND cached_window = nullptr;
                                static RECT last_rect{0, 0, 0, 0};
                                static auto last_handle_refresh = clock::time_point{};
                                static auto last_rect_poll = clock::time_point{};
                                static bool needs_sync = true;

                                constexpr auto kHandleRefreshInterval = std::chrono::seconds(2);
                                constexpr auto kRectPollInterval = std::chrono::milliseconds(16);

                                auto now = clock::now();
                                bool loader_visible = Overlay::get_loading_stage() != Overlay::LoadingStage::Ready;
                                bool menu_forces_visible = menu_visible;

                                if (!cached_window || (now - last_handle_refresh) >= kHandleRefreshInterval)
                                {
                                    cached_window = FindWindowA(nullptr, "Roblox");
                                    last_handle_refresh = now;
                                    needs_sync = true;
                                }

                                if (!cached_window)
                                {
                                    update_visibility(loader_visible || menu_forces_visible);
                                    return;
                                }

                                if (!needs_sync && (now - last_rect_poll) < kRectPollInterval)
                                    return;

                                RECT rect;
                                if (!GetWindowRect(cached_window, &rect))
                                {
                                    cached_window = nullptr;
                                    needs_sync = true;
                                    return;
                                }

                                last_rect_poll = now;

                                HWND foreground = GetForegroundWindow();
                                HWND foreground_root = foreground ? GetAncestor(foreground, GA_ROOT) : nullptr;
                                HWND effective_foreground = foreground_root ? foreground_root : foreground;
                                bool roblox_active = (effective_foreground == cached_window);
                                bool should_show_overlay = roblox_active || menu_forces_visible || loader_visible;
                                update_visibility(should_show_overlay);
                                if (!should_show_overlay)
                                    return;

                                if (!needs_sync &&
                                    rect.left == last_rect.left &&
                                    rect.top == last_rect.top &&
                                    rect.right == last_rect.right &&
                                    rect.bottom == last_rect.bottom)
                                {
                                    return;
                                }

                                int width = rect.right - rect.left;
                                int height = rect.bottom - rect.top;
                                if (width <= 0 || height <= 0)
                                    return;

                                last_rect = rect;
                                needs_sync = false;

                                SetWindowPos(
                                    overlayWindow,
                                    HWND_TOPMOST,
                                    rect.left,
                                    rect.top,
                                    width,
                                    height,
                                    SWP_NOACTIVATE | SWP_SHOWWINDOW);
                            }

                            void Overlay::updateInputPassthrough(bool capture_input)
                            {
                                if (!overlayWindow)
                                    return;

                                static bool last_capture_state = false;
                                if (capture_input == last_capture_state)
                                    return;

                                LONG_PTR ex_style = GetWindowLongPtr(overlayWindow, GWL_EXSTYLE);
                                if (capture_input)
                                {
                                    ex_style &= ~WS_EX_TRANSPARENT;
                                }
                                else
                                {
                                    ex_style |= WS_EX_TRANSPARENT;
                                }

                                SetWindowLongPtr(overlayWindow, GWL_EXSTYLE, ex_style);
                                SetWindowPos(
                                    overlayWindow,
                                    HWND_TOPMOST,
                                    0,
                                    0,
                                    0,
                                    0,
                                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
                                last_capture_state = capture_input;
                            }

                            void Overlay::render()
                            {
                                if (!device || !context)
                                    return;

                                updateWindowPosition();

                                if (resize_width != 0 && resize_height != 0)
                                {
                                    cleanupRenderTarget();
                                    swapchain->ResizeBuffers(0, resize_width, resize_height, DXGI_FORMAT_UNKNOWN, 0);
                                    resize_width = resize_height = 0;
                                    createRenderTarget();
                                }

                                ImGui_ImplDX11_NewFrame();
                                ImGui_ImplWin32_NewFrame();
                                ImGui::NewFrame();

                                auto stage = Overlay::get_loading_stage();
                                auto loading_text = Overlay::get_loading_message();
                                bool runtime_ready_flag = Overlay::is_runtime_ready();
                                bool loader_active = !(runtime_ready_flag && stage == Overlay::LoadingStage::Ready);
                                if (!loader_active)
                                    g_loader_capture_requested = false;

                                ImGuiIO &io = ImGui::GetIO();

                                static bool insert_prev_down = false;
                                bool insert_down = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
                                if (loader_active)
                                {
                                    if (!menu_visible)
                                    {
                                        menu_visible = true;
                                        showMenu = true;
                                        evo::theme::menu_open = true;
                                    }
                                }
                                else if (insert_down && !insert_prev_down)
                                {
                                    menu_visible = !menu_visible;
                                    showMenu = menu_visible;
                                    evo::theme::menu_open = menu_visible;
                                    if (!menu_visible)
                                        evo::reset_all_popups();
                                }
                                insert_prev_down = insert_down;

                                if (!loader_active && menu_visible != evo::theme::menu_open)
                                {
                                    menu_visible = evo::theme::menu_open;
                                    showMenu = menu_visible;
                                    if (!menu_visible)
                                        evo::reset_all_popups();
                                }

                                bool loader_capture = loader_active && g_loader_capture_requested;
                                updateInputPassthrough((menu_visible && !loader_active) || loader_capture);

                                io.MouseDrawCursor = menu_visible;

                                static std::array<bool, 256> key_state{};
                                auto &manager = modules::ModuleManager::get_instance();
                                for (int key = 1; key < 256; ++key)
                                {
                                    if (key == VK_INSERT)
                                        continue;

                                    bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
                                    if (down && !key_state[key])
                                        manager.on_key_press(key);
                                    else if (!down && key_state[key])
                                        manager.on_key_release(key);
                                    key_state[key] = down;
                                }

                                if (loader_active)
                                {
                                    RenderLoadingOverlay(stage, loading_text);
                                }
                                else
                                {
                                    cradle::engine::PlayerCache::update_cache();
                                    TickMovementOverrides();
                                    TickGravityOverride();
                                    TickLightingOverrides();

                                    static auto last_wall_cache_tick = std::chrono::steady_clock::time_point{};
                                    auto now = std::chrono::steady_clock::now();
                                    constexpr auto kWallCacheInterval = std::chrono::milliseconds(50);
                                    if (now - last_wall_cache_tick >= kWallCacheInterval)
                                    {
                                        auto dm_instance = cradle::engine::DataModel::get_instance();
                                        if (dm_instance.is_valid())
                                        {
                                            cradle::engine::Wallcheck::update_world_cache(dm_instance);
                                            last_wall_cache_tick = now;
                                        }
                                    }
                                    manager.update_all();
                                    manager.render_all();

                                    RenderCheatIndicator();

                                    if (menu_visible)
                                    {
                                        RenderEvoMenuWindow(*this);
                                    }

                                    cradle::config::ConfigManager::tick(manager);
                                }

                                auto render_start = std::chrono::high_resolution_clock::now();
                                ImGui::Render();
                                auto render_end = std::chrono::high_resolution_clock::now();

                                ImDrawData *draw_data = ImGui::GetDrawData();
                                double build_ms = std::chrono::duration<double, std::milli>(render_end - render_start).count();
                                int cmd_lists = draw_data ? draw_data->CmdListsCount : 0;
                                int total_cmds = 0;
                                int vertices = draw_data ? draw_data->TotalVtxCount : 0;
                                int indices = draw_data ? draw_data->TotalIdxCount : 0;
                                if (draw_data)
                                {
                                    for (int n = 0; n < draw_data->CmdListsCount; ++n)
                                    {
                                        if (!draw_data->CmdLists[n])
                                            continue;
                                        total_cmds += draw_data->CmdLists[n]->CmdBuffer.Size;
                                    }
                                }

                                static double draw_accum_ms = 0.0;
                                static double draw_accum_cmd_lists = 0.0;
                                static double draw_accum_total_cmds = 0.0;
                                static double draw_accum_vertices = 0.0;
                                static double draw_accum_indices = 0.0;
                                static int draw_samples = 0;
                                static auto draw_last_flush = std::chrono::steady_clock::now();
                                static auto draw_last_publish = std::chrono::steady_clock::time_point{};
                                constexpr auto kDrawFlushInterval = std::chrono::milliseconds(250);
                                constexpr auto kDrawIdleInterval = std::chrono::milliseconds(1000);

                                draw_accum_ms += build_ms;
                                draw_accum_cmd_lists += cmd_lists;
                                draw_accum_total_cmds += total_cmds;
                                draw_accum_vertices += vertices;
                                draw_accum_indices += indices;
                                ++draw_samples;

                                auto draw_now = std::chrono::steady_clock::now();
                                bool published_draw_snapshot = false;
                                if (draw_now - draw_last_flush >= kDrawFlushInterval)
                                {
                                    if (draw_samples > 0)
                                    {
                                        double avg_ms = draw_accum_ms / static_cast<double>(draw_samples);
                                        int avg_cmd_lists = static_cast<int>((draw_accum_cmd_lists / static_cast<double>(draw_samples)) + 0.5);
                                        int avg_total_cmds = static_cast<int>((draw_accum_total_cmds / static_cast<double>(draw_samples)) + 0.5);
                                        int avg_vertices = static_cast<int>((draw_accum_vertices / static_cast<double>(draw_samples)) + 0.5);
                                        int avg_indices = static_cast<int>((draw_accum_indices / static_cast<double>(draw_samples)) + 0.5);
                                        cradle::profiling::record_draw_stats(avg_ms, avg_cmd_lists, avg_total_cmds, avg_vertices, avg_indices);
                                        draw_last_publish = draw_now;
                                        published_draw_snapshot = true;
                                    }

                                    draw_accum_ms = 0.0;
                                    draw_accum_cmd_lists = 0.0;
                                    draw_accum_total_cmds = 0.0;
                                    draw_accum_vertices = 0.0;
                                    draw_accum_indices = 0.0;
                                    draw_samples = 0;
                                    draw_last_flush = draw_now;
                                }

                                if (!published_draw_snapshot &&
                                    draw_last_publish != std::chrono::steady_clock::time_point{} &&
                                    draw_now - draw_last_publish >= kDrawIdleInterval)
                                {
                                    cradle::profiling::record_draw_stats(0.0, 0, 0, 0, 0);
                                    draw_last_publish = std::chrono::steady_clock::time_point{};
                                }

                                const float clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                                context->OMSetRenderTargets(1, &renderTargetView, nullptr);
                                context->ClearRenderTargetView(renderTargetView, clear_color);
                                if (draw_data)
                                    ImGui_ImplDX11_RenderDrawData(draw_data);
                                swapchain->Present(1, 0);
                            }

                            bool Overlay::isRunning()
                            {
                                MSG msg;
                                while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
                                {
                                    TranslateMessage(&msg);
                                    DispatchMessage(&msg);
                                    if (msg.message == WM_QUIT)
                                        return false;
                                }
                                return true;
                            }

                            HWND Overlay::get_overlay_window()
                            {
                                return instance ? instance->overlayWindow : nullptr;
                            }

                            POINT Overlay::get_overlay_origin()
                            {
                                POINT origin{0, 0};
                                if (instance && instance->overlayWindow)
                                {
                                    RECT rect;
                                    if (GetWindowRect(instance->overlayWindow, &rect))
                                    {
                                        origin.x = rect.left;
                                        origin.y = rect.top;
                                    }
                                }
                                return origin;
                            }

                            LRESULT CALLBACK Overlay::windowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
                            {
                                if (menu_visible)
                                {
                                    switch (msg)
                                    {
                                    case WM_MOUSEMOVE:
                                        evo::_input->set_mouse_position(evo::vec2_t(
                                            static_cast<float>(GET_X_LPARAM(lparam)),
                                            static_cast<float>(GET_Y_LPARAM(lparam))));
                                        break;
                                    case WM_MOUSEWHEEL:
                                        evo::_input->set_mouse_wheel(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / static_cast<float>(WHEEL_DELTA));
                                        break;
                                    default:
                                        break;
                                    }

                                    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam))
                                        return true;
                                }

                                switch (msg)
                                {
                                case WM_MOUSEACTIVATE:
                                    if (!menu_visible)
                                        return MA_NOACTIVATE;
                                    break;
                                case WM_SIZE:
                                    if (wparam != SIZE_MINIMIZED)
                                    {
                                        resize_width = (UINT)LOWORD(lparam);
                                        resize_height = (UINT)HIWORD(lparam);
                                    }
                                    return 0;
                                case WM_DESTROY:
                                    PostQuitMessage(0);
                                    return 0;
                                }

                                return DefWindowProc(hwnd, msg, wparam, lparam);
                            }

                            void Overlay::cleanup()
                            {
                                cleanupRenderTarget();
                                ImGui_ImplDX11_Shutdown();
                                ImGui_ImplWin32_Shutdown();
                                if (ImGui::GetCurrentContext())
                                    ImGui::DestroyContext();

                                if (swapchain)
                                {
                                    swapchain->Release();
                                    swapchain = nullptr;
                                }
                                if (renderTargetView)
                                {
                                    renderTargetView->Release();
                                    renderTargetView = nullptr;
                                }
                                if (context)
                                {
                                    context->Release();
                                    context = nullptr;
                                }
                                if (device)
                                {
                                    device->Release();
                                    device = nullptr;
                                }
                                if (overlayWindow)
                                {
                                    DestroyWindow(overlayWindow);
                                    overlayWindow = nullptr;
                                }

                                UnregisterClassW(kOverlayWindowClass, GetModuleHandle(nullptr));
                                instance = nullptr;
                            }
                        } // namespace overlay
                    } // namespace cradle
