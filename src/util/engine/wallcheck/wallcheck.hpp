#pragma once
#include "../instance/instance.hpp"
#include "../datamodel/datamodel.hpp"
#include "../math.hpp"
#include "../../memory/memory.hpp"
#include "../offsets.hpp"
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <mutex>
#include <cfloat>
#include <unordered_map>
#include <memory>

namespace cradle::engine
{
    struct WorldPart
    {
        vector3 pos;
        vector3 size;
        matrix3 rot;
        float vol;
        bool large;
        std::uint64_t primitive{0};
        float transparency{0.0f};
        bool can_collide{true};
        vector3 padded_half_extents;
        vector3 los_half_extents;
        float bounding_radius{0.0f};
    };

    class Wallcheck
    {
    public:
        struct VisibilityDebug
        {
            int rays_enqueued = 0;
            int rays_checked = 0;
            float visible_score = 0.0f;
            float required_score = 0.0f;
            float height_delta = 0.0f;
            float target_distance = 0.0f;
            bool head_override = false;
            bool fallback_used = false;
            bool lateral_probe = false;
            int candidate_parts = 0;
            float head_score = 0.0f;
            float confidence = 0.0f;
            bool final_result = false;
        };

    private:
    static std::shared_ptr<const std::vector<WorldPart>> geometry_snapshot;
    static std::atomic<double> last_refresh;
        static std::atomic<bool> building;
        static std::atomic<bool> ready;
        static std::unordered_map<std::uint64_t, std::pair<bool, std::chrono::steady_clock::time_point>> vis_cache;
        static std::mutex vis_cache_mtx;
        static std::unordered_map<std::uint64_t, VisibilityDebug> debug_snapshots;
        static std::mutex debug_snapshots_mtx;
        static std::atomic<bool> debug_enabled;

    public:
        static void update_world_cache(DataModel &dm);
    static bool is_visible(const vector3 &from, const vector3 &head, const vector3 &torso,
                   const vector3 &pelvis, std::uint64_t player_addr = 0);
        static bool is_cache_ready()
        {
            auto snapshot = std::atomic_load(&geometry_snapshot);
            return ready && snapshot && !snapshot->empty();
        }
    static void force_cache_refresh() { last_refresh.store(0.0); }
        static const std::vector<WorldPart> &get_world_parts()
        {
            static const std::vector<WorldPart> empty;
            auto snapshot = std::atomic_load(&geometry_snapshot);
            return snapshot ? *snapshot : empty;
        }
        static bool line_of_sight(const vector3 &origin, const vector3 &target, float max_distance_override = 0.0f);
        static void set_debug_enabled(bool enabled);
        static bool get_debug_snapshot(std::uint64_t player_addr, VisibilityDebug &out);
    };
}
