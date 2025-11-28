#pragma once

#include <cstddef>
#include <string>

namespace cradle::profiling
{
    struct ProfilerSnapshot
    {
        double esp_refresh_ms = 0.0;
        double esp_attempts = 0.0;
        double esp_success = 0.0;

        double playercache_ms = 0.0;
        std::size_t playercache_players = 0;

        double draw_build_ms = 0.0;
        int draw_cmd_lists = 0;
        int draw_total_cmds = 0;
        int draw_vertices = 0;
        int draw_indices = 0;

        double module_update_total_ms = 0.0;
        double module_update_peak_ms = 0.0;
        std::string module_update_peak_name;

        double module_render_total_ms = 0.0;
        double module_render_peak_ms = 0.0;
        std::string module_render_peak_name;
    };

    void record_esp_frame(double refresh_ms, double attempts, double success);
    void record_playercache(double update_ms, std::size_t player_count);
    void record_draw_stats(double build_ms, int cmd_lists, int total_cmds, int vertices, int indices);
    void record_module_update_stats(double total_ms, double peak_ms, const std::string &peak_name);
    void record_module_render_stats(double total_ms, double peak_ms, const std::string &peak_name);

    ProfilerSnapshot get_snapshot();
}
