#include "profiler.hpp"

#include <mutex>

namespace cradle::profiling
{
    namespace
    {
        std::mutex g_profiler_mutex;
        ProfilerSnapshot g_snapshot;
    }

    void record_esp_frame(double refresh_ms, double attempts, double success)
    {
        std::lock_guard<std::mutex> lock(g_profiler_mutex);
        g_snapshot.esp_refresh_ms = refresh_ms;
        g_snapshot.esp_attempts = attempts;
        g_snapshot.esp_success = success;
    }

    void record_playercache(double update_ms, std::size_t player_count)
    {
        std::lock_guard<std::mutex> lock(g_profiler_mutex);
        g_snapshot.playercache_ms = update_ms;
        g_snapshot.playercache_players = player_count;
    }

    void record_draw_stats(double build_ms, int cmd_lists, int total_cmds, int vertices, int indices)
    {
        std::lock_guard<std::mutex> lock(g_profiler_mutex);
        g_snapshot.draw_build_ms = build_ms;
        g_snapshot.draw_cmd_lists = cmd_lists;
        g_snapshot.draw_total_cmds = total_cmds;
        g_snapshot.draw_vertices = vertices;
        g_snapshot.draw_indices = indices;
    }

    void record_module_update_stats(double total_ms, double peak_ms, const std::string &peak_name)
    {
        std::lock_guard<std::mutex> lock(g_profiler_mutex);
        g_snapshot.module_update_total_ms = total_ms;
        g_snapshot.module_update_peak_ms = peak_ms;
        g_snapshot.module_update_peak_name = peak_name;
    }

    void record_module_render_stats(double total_ms, double peak_ms, const std::string &peak_name)
    {
        std::lock_guard<std::mutex> lock(g_profiler_mutex);
        g_snapshot.module_render_total_ms = total_ms;
        g_snapshot.module_render_peak_ms = peak_ms;
        g_snapshot.module_render_peak_name = peak_name;
    }

    ProfilerSnapshot get_snapshot()
    {
        std::lock_guard<std::mutex> lock(g_profiler_mutex);
        return g_snapshot;
    }
}
