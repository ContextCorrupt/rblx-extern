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
#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <functional>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <dwmapi.h>
#include <limits>
#include "../util/engine/visualengine/visualengine.hpp"
#include "../util/engine/datamodel/datamodel.hpp"
#include "../util/engine/instance/instance.hpp"
#include "../util/engine/wallcheck/wallcheck.hpp"
#include "../util/engine/offsets.hpp"
#include "../cache/player_cache.hpp"
#include "../modules/module_manager.hpp"
#include "../modules/friend_manager.hpp"
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
                Gravity
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
                                void RenderVisualColorPanel(evo::child_t *child);
                                void RenderProfilingPanel(evo::child_t *child, cradle::modules::Module *module);
                                void RenderConfigTab(evo::window_t *window);
                                void RenderTabContents(evo::window_t *window, EvoTab tab);
                                void RenderCheatIndicator();

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

                            bool Overlay::initialize()
                            {
                                if (instance)
                                    return true;

                                instance = this;
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

                                updateInputPassthrough();
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

                                using clock = std::chrono::steady_clock;
                                static HWND cached_window = nullptr;
                                static RECT last_rect{0, 0, 0, 0};
                                static auto last_handle_refresh = clock::time_point{};
                                static auto last_rect_poll = clock::time_point{};
                                static bool needs_sync = true;

                                constexpr auto kHandleRefreshInterval = std::chrono::seconds(2);
                                constexpr auto kRectPollInterval = std::chrono::milliseconds(16);

                                auto now = clock::now();

                                if (!cached_window || (now - last_handle_refresh) >= kHandleRefreshInterval)
                                {
                                    cached_window = FindWindowA(nullptr, "Roblox");
                                    last_handle_refresh = now;
                                    needs_sync = true;
                                }

                                if (!cached_window)
                                    return;

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

                            void Overlay::updateInputPassthrough()
                            {
                                if (!overlayWindow)
                                    return;

                                LONG_PTR ex_style = GetWindowLongPtr(overlayWindow, GWL_EXSTYLE);
                                if (menu_visible)
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

                                ImGuiIO &io = ImGui::GetIO();
                                io.MouseDrawCursor = menu_visible;

                                static bool insert_prev_down = false;
                                bool insert_down = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
                                if (insert_down && !insert_prev_down)
                                {
                                    menu_visible = !menu_visible;
                                    showMenu = menu_visible;
                                    evo::theme::menu_open = menu_visible;
                                    if (!menu_visible)
                                        evo::reset_all_popups();
                                    updateInputPassthrough();
                                }
                                insert_prev_down = insert_down;

                                if (menu_visible != evo::theme::menu_open)
                                {
                                    menu_visible = evo::theme::menu_open;
                                    showMenu = menu_visible;
                                    if (!menu_visible)
                                        evo::reset_all_popups();
                                    updateInputPassthrough();
                                }

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

                                cradle::engine::PlayerCache::update_cache();
                                TickMovementOverrides();
                                TickGravityOverride();

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
