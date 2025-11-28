#include "profiling_module.hpp"

#include <imgui.h>
#include <cstdio>

#include "../../util/profiling/profiler.hpp"

namespace cradle::modules
{
    ProfilingModule::ProfilingModule()
        : Module("profiling", "diagnostic overlay controls")
    {
        enabled = true;
        settings.push_back(Setting("show esp cache profiler", false));
        settings.push_back(Setting("show player cache profiler", false));
        settings.push_back(Setting("show draw profiler", false));
    settings.push_back(Setting("show module profiler", false));
    }

    void ProfilingModule::on_render()
    {
        auto esp_setting = get_setting("show esp cache profiler");
        auto player_setting = get_setting("show player cache profiler");
        auto draw_setting = get_setting("show draw profiler");
        auto module_setting = get_setting("show module profiler");

        const bool show_esp = esp_setting && esp_setting->value.bool_val;
        const bool show_player = player_setting && player_setting->value.bool_val;
        const bool show_draw = draw_setting && draw_setting->value.bool_val;
        const bool show_modules = module_setting && module_setting->value.bool_val;

        if (!(show_esp || show_player || show_draw || show_modules))
            return;

        ImGuiViewport *viewport = ImGui::GetMainViewport();
        if (!viewport)
            return;
        ImDrawList *draw_list = ImGui::GetBackgroundDrawList();
        if (!draw_list)
            return;

        auto snapshot = cradle::profiling::get_snapshot();
        float y_offset = 32.0f;
        constexpr float line_step = 18.0f;

        char buffer[192];

        if (show_esp)
        {
            std::snprintf(buffer, sizeof(buffer),
                          "ESP profiler · avg refresh %.3f ms · attempts %.1f/s · success %.1f/s",
                          snapshot.esp_refresh_ms,
                          snapshot.esp_attempts,
                          snapshot.esp_success);
            draw_list->AddText(viewport->Pos + ImVec2(32.0f, y_offset), IM_COL32(80, 200, 255, 230), buffer);
            y_offset += line_step;
        }

        if (show_player)
        {
            std::snprintf(buffer, sizeof(buffer),
                          "Player cache · refresh %.2f ms · players %zu",
                          snapshot.playercache_ms,
                          snapshot.playercache_players);
            draw_list->AddText(viewport->Pos + ImVec2(32.0f, y_offset), IM_COL32(255, 210, 80, 230), buffer);
            y_offset += line_step;
        }

        if (show_draw)
        {
            std::snprintf(buffer, sizeof(buffer),
                          "Draw build · %.3f ms · cmd lists %d (%d cmds) · vtx %d · idx %d",
                          snapshot.draw_build_ms,
                          snapshot.draw_cmd_lists,
                          snapshot.draw_total_cmds,
                          snapshot.draw_vertices,
                          snapshot.draw_indices);
            draw_list->AddText(viewport->Pos + ImVec2(32.0f, y_offset), IM_COL32(180, 255, 120, 230), buffer);
            y_offset += line_step;
        }

        if (show_modules)
        {
            std::snprintf(buffer, sizeof(buffer),
                          "Modules update · total %.3f ms · slowest %s (%.3f ms)",
                          snapshot.module_update_total_ms,
                          snapshot.module_update_peak_name.empty() ? "-" : snapshot.module_update_peak_name.c_str(),
                          snapshot.module_update_peak_ms);
            draw_list->AddText(viewport->Pos + ImVec2(32.0f, y_offset), IM_COL32(255, 150, 230, 230), buffer);
            y_offset += line_step;

            std::snprintf(buffer, sizeof(buffer),
                          "Modules render · total %.3f ms · slowest %s (%.3f ms)",
                          snapshot.module_render_total_ms,
                          snapshot.module_render_peak_name.empty() ? "-" : snapshot.module_render_peak_name.c_str(),
                          snapshot.module_render_peak_ms);
            draw_list->AddText(viewport->Pos + ImVec2(32.0f, y_offset), IM_COL32(255, 120, 180, 230), buffer);
        }
    }
}
