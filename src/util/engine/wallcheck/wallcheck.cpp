#include "wallcheck.hpp"
#include <algorithm>
#include <cmath>
#include <vector>
#include <array>
#include <cstdint>

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace cradle::engine
{
    namespace
    {
        constexpr auto kMinGeometryRefreshInterval = std::chrono::milliseconds(400);
        constexpr double kMinGeometryRefreshSeconds = kMinGeometryRefreshInterval.count() / 1000.0;
    }

    std::shared_ptr<const std::vector<WorldPart>> Wallcheck::geometry_snapshot;
    std::atomic<double> Wallcheck::last_refresh{0.0};
    std::atomic<bool> Wallcheck::building{false};
    std::atomic<bool> Wallcheck::ready{false};
    std::unordered_map<std::uint64_t, std::pair<bool, std::chrono::steady_clock::time_point>> Wallcheck::vis_cache;
    std::mutex Wallcheck::vis_cache_mtx;
    std::unordered_map<std::uint64_t, Wallcheck::VisibilityDebug> Wallcheck::debug_snapshots;
    std::mutex Wallcheck::debug_snapshots_mtx;
    std::atomic<bool> Wallcheck::debug_enabled{false};

    static inline bool check_ray_box(const vector3 &origin, const vector3 &dir, const WorldPart &part, float dist, float *hit_distance = nullptr)
    {
        vector3 to_part = part.pos - origin;
        float radius = part.size.magnitude() * 0.866f;
        float dist_sq = to_part.X * to_part.X + to_part.Y * to_part.Y + to_part.Z * to_part.Z;
        if (dist_sq > (radius + dist) * (radius + dist))
            return false;

        matrix3 inv = part.rot.transpose();
        vector3 local_o = inv.multiply(origin - part.pos);
        vector3 local_d = inv.multiply(dir);

        vector3 half = part.size * 0.5f;
        float tmin = -FLT_MAX, tmax = FLT_MAX;

        for (int i = 0; i < 3; i++)
        {
            float o = (i == 0) ? local_o.X : (i == 1) ? local_o.Y
                                                      : local_o.Z;
            float d = (i == 0) ? local_d.X : (i == 1) ? local_d.Y
                                                      : local_d.Z;
            float h = (i == 0) ? half.X : (i == 1) ? half.Y
                                                   : half.Z;

            if (std::fabs(d) < 1e-6f)
            {
                if (o < -h || o > h)
                    return false;
            }
            else
            {
                float t1 = (-h - o) / d;
                float t2 = (h - o) / d;
                if (t1 > t2)
                    std::swap(t1, t2);
                if (t1 > tmin)
                    tmin = t1;
                if (t2 < tmax)
                    tmax = t2;
                if (tmin > tmax || tmax < 0.0f || tmin > dist)
                    return false;
            }
        }
        if (!((tmin > 0.0f || tmax > 0.0f) && tmin <= dist && tmin <= tmax))
            return false;

        float hit = tmin;
        if (hit < 0.0f)
            hit = tmax;
        if (hit < 0.0f)
            hit = 0.0f;
        if (hit > dist)
            return false;

        if (hit_distance)
            *hit_distance = hit;
        return true;
    }

    static inline bool is_micro_occluder(const WorldPart &part)
    {
        const float sx = std::fabs(part.size.X);
        const float sy = std::fabs(part.size.Y);
        const float sz = std::fabs(part.size.Z);

        float max_extent = sx;
        max_extent = std::max(max_extent, sy);
        max_extent = std::max(max_extent, sz);

        float min_extent = sx;
        min_extent = std::min(min_extent, sy);
        min_extent = std::min(min_extent, sz);

        float mid_extent = sx + sy + sz - max_extent - min_extent;

        const float face_xy = sx * sy;
        const float face_yz = sy * sz;
        const float face_xz = sx * sz;
        const float largest_face = std::max(face_xy, std::max(face_yz, face_xz));

        const bool extremely_compact = (max_extent < 0.18f) || (largest_face < 0.03f);
        const bool thin_panel = (max_extent < 0.5f && mid_extent < 0.35f && largest_face < 0.12f);
        const bool lightweight = (part.vol < 2.2f && max_extent < 0.35f);
        const bool sliver = (largest_face < 0.06f && max_extent < 0.65f);

        return extremely_compact || thin_panel || lightweight || sliver;
    }

    void Wallcheck::update_world_cache(DataModel &dm)
    {
        auto now = std::chrono::high_resolution_clock::now();
        double now_seconds = std::chrono::duration<double>(now.time_since_epoch()).count();
        double last_seconds = last_refresh.load();
        if (last_seconds > 0.0 && (now_seconds - last_seconds) < kMinGeometryRefreshSeconds)
        {
            if (ready.load())
                return;
        }

        bool expected = false;
        if (!building.compare_exchange_strong(expected, true))
            return;

        std::uint64_t dm_addr = dm.address;

        std::thread([dm_addr]() {
            struct BuildReset
            {
                ~BuildReset()
                {
                    double stamp = std::chrono::duration<double>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
                    Wallcheck::last_refresh.store(stamp);
                    Wallcheck::building.store(false);
                }
            } reset_guard;

            DataModel dm_local(dm_addr);
            std::vector<WorldPart> parts;
            parts.reserve(3000);

            Instance workspace = dm_local.get_workspace();
            if (!workspace.is_valid()) {
                std::atomic_store(&Wallcheck::geometry_snapshot, std::shared_ptr<const std::vector<WorldPart>>{});
                ready = true;
                return;
            }

            Instance players = dm_local.get_players();
            std::vector<Instance> player_characters;
            if (players.is_valid()) {
                for (auto &player : players.get_children()) {
                    std::uint64_t char_addr = cradle::memory::read<std::uint64_t>(player.address + Offsets::Player::ModelInstance);
                    if (char_addr > 0x10000) {
                        player_characters.push_back(Instance(char_addr));
                    }
                }
            }

            std::string classes[] = {"Part", "MeshPart", "UnionOperation"};

            for (const auto &cls : classes) {
                for (auto &part : workspace.find_descendants_of_class(cls)) {
                    if (!part.is_valid()) continue;

                    bool is_player_part = false;
                    for (auto &character : player_characters) {
                        if (part.is_descendant_of(character)) {
                            is_player_part = true;
                            break;
                        }
                    }
                    if (is_player_part) continue;

                    std::uint64_t primitive = cradle::memory::read<std::uint64_t>(part.address + Offsets::BasePart::Primitive);
                    if (primitive < 0x10000) continue;

                    float transparency = cradle::memory::read<float>(primitive + Offsets::BasePart::Transparency);
                    if (transparency > 0.995f) continue;

                    std::uint8_t primitive_flags = cradle::memory::read<std::uint8_t>(primitive + Offsets::BasePart::PrimitiveFlags);
                    bool can_collide = (primitive_flags & Offsets::PrimitiveFlags::CanCollide) != 0;

                    vector3 size = cradle::memory::read<vector3>(primitive + Offsets::BasePart::Size);
                    float vol = size.X * size.Y * size.Z;
                    if (vol < 0.5f || vol > 8000000.0f) continue;

                    cframe cf = cradle::memory::read<cframe>(primitive + Offsets::BasePart::Rotation);
                    vector3 padded_half = size * 0.5f + vector3(1.0f, 1.0f, 1.0f);
                    vector3 los_half = size * 0.5f + vector3(0.8f, 0.8f, 0.8f);
                    float radius = size.magnitude() * 0.6f + 0.5f;
                    parts.push_back({cf.position, size, cf.rotation, vol, vol > 10.0f, primitive,
                                     transparency, can_collide, padded_half, los_half, radius});
                }
            }

            auto new_geometry = std::make_shared<std::vector<WorldPart>>(std::move(parts));
            std::shared_ptr<const std::vector<WorldPart>> const_geometry(new_geometry);
            std::atomic_store(&Wallcheck::geometry_snapshot, const_geometry);
            {
                std::lock_guard<std::mutex> lk2(Wallcheck::vis_cache_mtx);
                Wallcheck::vis_cache.clear();
            }

            ready = true;

        }).detach();
    }

    bool Wallcheck::is_visible(const vector3 &from, const vector3 &head, const vector3 &torso,
                               const vector3 &pelvis, std::uint64_t player_addr)
    {
        if (!ready)
            return true;

        auto local_geometry = std::atomic_load(&geometry_snapshot);
        if (!local_geometry || local_geometry->empty())
            return true;

        if (player_addr != 0)
        {
            std::lock_guard<std::mutex> vlk(vis_cache_mtx);
            auto cached = vis_cache.find(player_addr);
            if (cached != vis_cache.end())
            {
                auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - cached->second.second)
                               .count();
                if (age < 100)
                    return cached->second.first;
            }
        }

        struct RaySample
        {
            vector3 origin;
            vector3 dir;
            float dist;
            std::uint8_t mask;
            float weight;
            bool lateral;
        };

    constexpr std::uint8_t kRegionHead = 1 << 0;
    constexpr std::uint8_t kRegionTorso = 1 << 1;
    constexpr std::uint8_t kRegionPelvis = 1 << 2;

        std::vector<RaySample> rays;
        rays.reserve(96);
        float max_sample_distance = 0.0f;

        auto push_ray = [&](const vector3 &origin, const vector3 &target, std::uint8_t mask, float weight,
                            float surface_bias, bool lateral_flag) {
            vector3 diff = target - origin;
            float dist = diff.magnitude();
            if (dist <= 0.05f)
                return;
            vector3 dir = diff / dist;
            if (surface_bias > 0.0f)
            {
                float capped_bias = std::min(surface_bias, std::max(0.05f, dist * 0.35f));
                capped_bias = std::min(capped_bias, std::max(0.0f, dist * 0.6f));
                vector3 biased = target + dir * capped_bias;
                diff = biased - origin;
                dist = diff.magnitude();
                if (dist <= 0.05f)
                    return;
                dir = diff / dist;
            }
            if (dist > max_sample_distance)
                max_sample_distance = dist;
            rays.push_back({origin, dir, dist, mask, weight, lateral_flag});
        };

        const std::array<float, 3> origin_offsets = {0.0f, 1.3f, -1.15f};
        const std::array<vector3, 4> lateral_origins = {
            vector3(1.35f, 0.0f, 0.0f),
            vector3(-1.35f, 0.0f, 0.0f),
            vector3(0.0f, 0.0f, 1.35f),
            vector3(0.0f, 0.0f, -1.35f)};

        auto queue_target = [&](const vector3 &target, std::uint8_t mask, float weight, float surface_bias,
                                bool multi_origin, bool include_lateral) {
            if (multi_origin)
            {
                for (float offset : origin_offsets)
                {
                    vector3 origin = from;
                    origin.Y += offset;
                    push_ray(origin, target, mask, weight, surface_bias, false);
                }
            }
            else
            {
                push_ray(from, target, mask, weight, surface_bias, false);
            }

            if (include_lateral)
            {
                for (const auto &offset : lateral_origins)
                {
                    vector3 origin = from + offset;
                    push_ray(origin, target, mask, weight * 0.65f, surface_bias, true);
                }
            }
        };

        auto emit_region = [&](const vector3 &base, float lateral_offset, float vertical_offset, bool include_diag,
                               float surface_bias, float weight, std::uint8_t mask, bool multi_origin, bool include_lateral) {
            std::vector<vector3> points;
            points.push_back(base);
            if (lateral_offset > 0.0f)
            {
                points.push_back(base + vector3(lateral_offset, 0.0f, 0.0f));
                points.push_back(base + vector3(-lateral_offset, 0.0f, 0.0f));
                points.push_back(base + vector3(0.0f, 0.0f, lateral_offset));
                points.push_back(base + vector3(0.0f, 0.0f, -lateral_offset));
            }
            if (vertical_offset > 0.0f)
            {
                points.push_back(base + vector3(0.0f, vertical_offset, 0.0f));
                points.push_back(base + vector3(0.0f, -vertical_offset, 0.0f));
            }
            if (include_diag && lateral_offset > 0.0f)
            {
                points.push_back(base + vector3(lateral_offset, lateral_offset, 0.0f));
                points.push_back(base + vector3(-lateral_offset, lateral_offset, 0.0f));
                points.push_back(base + vector3(lateral_offset, -lateral_offset, 0.0f));
                points.push_back(base + vector3(-lateral_offset, -lateral_offset, 0.0f));
                points.push_back(base + vector3(lateral_offset, 0.0f, lateral_offset));
                points.push_back(base + vector3(-lateral_offset, 0.0f, lateral_offset));
                points.push_back(base + vector3(lateral_offset, 0.0f, -lateral_offset));
                points.push_back(base + vector3(-lateral_offset, 0.0f, -lateral_offset));
            }

            for (const auto &pt : points)
                queue_target(pt, mask, weight, surface_bias, multi_origin, include_lateral);
        };

    float target_distance = (torso - from).magnitude();
    bool allow_diag = target_distance > 9.0f;
    bool allow_wide_diag = target_distance > 24.0f;
    bool allow_head_lateral = target_distance > 14.0f;
    bool allow_upper_head_lateral = target_distance > 32.0f;
    bool allow_torso_lateral = target_distance > 26.0f;
    emit_region(head, allow_diag ? 0.25f : 0.18f, allow_diag ? 0.2f : 0.12f, allow_diag, 0.35f, 2.2f, kRegionHead, true, allow_head_lateral);
    emit_region(head + vector3(0.0f, 0.35f, 0.0f), allow_diag ? 0.2f : 0.15f, 0.0f, allow_wide_diag, 0.3f, 1.7f, kRegionHead, true, allow_upper_head_lateral);
    emit_region(head + vector3(0.0f, -0.3f, 0.0f), allow_diag ? 0.2f : 0.12f, 0.0f, allow_wide_diag, 0.3f, 1.7f, kRegionHead, true, allow_upper_head_lateral);

    emit_region(torso, allow_diag ? 0.32f : 0.26f, allow_diag ? 0.28f : 0.18f, allow_diag, 0.25f, 1.4f, kRegionTorso, true, allow_torso_lateral);
    emit_region(torso + vector3(0.0f, 0.35f, 0.0f), allow_diag ? 0.28f : 0.22f, 0.0f, allow_wide_diag, 0.25f, 1.1f, kRegionTorso, true, target_distance > 48.0f);
    emit_region(pelvis, allow_diag ? 0.28f : 0.2f, allow_diag ? 0.2f : 0.12f, allow_diag, 0.2f, 1.0f, kRegionPelvis, false, target_distance > 55.0f);

    float visible_score = 0.0f;
    float head_score = 0.0f;
    float central_score = 0.0f;
    std::uint8_t region_hits = 0;
    bool head_override = false;
    bool central_override = false;
    bool lateral_success = false;
    int checked = 0;
    int total_considered = std::min<int>(72, static_cast<int>(rays.size()));
    float height_delta = head.Y - from.Y;
    float required_visible = 0.0f;

        if (rays.empty())
            return true;

        std::sort(rays.begin(), rays.end(), [](const RaySample &a, const RaySample &b) {
            return a.dist < b.dist;
        });

        auto dot = [](const vector3 &a, const vector3 &b) -> float {
            return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
        };

        vector3 primary_axis = head - from;
        float primary_axis_len = primary_axis.magnitude();
        if (primary_axis_len > 1e-3f)
            primary_axis = primary_axis / primary_axis_len;
        else
            primary_axis = vector3(0.0f, 1.0f, 0.0f);

        vector3 seg_min(std::min(from.X, head.X), std::min(from.Y, head.Y), std::min(from.Z, head.Z));
        vector3 seg_max(std::max(from.X, head.X), std::max(from.Y, head.Y), std::max(from.Z, head.Z));
        float seg_margin = std::clamp(target_distance * 0.25f, 6.0f, 36.0f);
        vector3 expand(seg_margin, std::max(5.0f, seg_margin * 0.6f), seg_margin);
        vector3 query_min = seg_min - expand;
        vector3 query_max = seg_max + expand;

        float padded_max_dist = std::max(max_sample_distance, primary_axis_len) + 6.0f;
        float lateral_allowance = 4.5f + 0.5f * std::max(0.0f, primary_axis_len / 60.0f);

        thread_local std::vector<const WorldPart *> candidate_parts_storage;
        auto &candidate_parts = candidate_parts_storage;
        candidate_parts.clear();
        candidate_parts.reserve(256);
        for (const auto &part : *local_geometry)
        {
            if (is_micro_occluder(part))
                continue;
            vector3 part_min = part.pos - part.padded_half_extents;
            vector3 part_max = part.pos + part.padded_half_extents;
            if (part_max.X < query_min.X || part_min.X > query_max.X ||
                part_max.Y < query_min.Y || part_min.Y > query_max.Y ||
                part_max.Z < query_min.Z || part_min.Z > query_max.Z)
                continue;
            vector3 to_part = part.pos - from;
            float forward = dot(to_part, primary_axis);
            float radius = part.bounding_radius;
            if (forward + radius < -6.5f)
                continue;
            if (forward - radius > padded_max_dist)
                continue;
            float to_part_len_sq = dot(to_part, to_part);
            float lateral_sq = to_part_len_sq - forward * forward;
            float lateral_limit = radius + lateral_allowance;
            if (lateral_sq > lateral_limit * lateral_limit)
                continue;
            candidate_parts.push_back(&part);
        }

        bool use_candidates = false;
        int candidate_reference = static_cast<int>(local_geometry->size());
        if (!candidate_parts.empty() && candidate_parts.size() < local_geometry->size())
        {
            use_candidates = true;
            candidate_reference = static_cast<int>(candidate_parts.size());
        }

        float base_ratio = 0.09f;
        if (target_distance > 120.0f)
            base_ratio = 0.03f;
        else if (target_distance > 80.0f)
            base_ratio = 0.05f;
        else if (target_distance > 40.0f)
            base_ratio = 0.07f;

        float density_scale = candidate_reference > 0 ? std::clamp(static_cast<float>(candidate_reference) / 160.0f, 0.3f, 1.0f) : 0.3f;
        base_ratio *= density_scale;
        base_ratio = std::clamp(base_ratio, 0.015f, 0.55f);

        required_visible = std::max(1.0f, std::ceil(total_considered * base_ratio));
        float max_visibility_goal = 2.4f;
        if (target_distance > 140.0f)
            max_visibility_goal = 1.15f;
        else if (target_distance > 110.0f)
            max_visibility_goal = 1.3f;
        else if (target_distance > 80.0f)
            max_visibility_goal = 1.6f;
        else if (target_distance > 50.0f)
            max_visibility_goal = 2.0f;
        required_visible = std::min(required_visible, max_visibility_goal);

        auto is_ray_clear = [&](const vector3 &origin, const vector3 &dir, float dist) -> bool {
            auto test_part = [&](const WorldPart &part) -> bool {
                if (!part.can_collide)
                    return true;
                if (part.transparency >= 0.65f)
                    return true;
                if (is_micro_occluder(part))
                    return true;
                if (part.vol <= 0.0f || part.size.X <= 0.0f || part.size.Y <= 0.0f || part.size.Z <= 0.0f)
                    return true;
                if (dist > 80.0f)
                {
                    float volume_gate = 60.0f + std::min(dist, 240.0f) * 0.45f;
                    float radius_gate = 1.75f + std::min(dist, 220.0f) * 0.004f;
                    if (part.vol < volume_gate && part.bounding_radius < radius_gate)
                        return true;
                }
                else if (dist > 45.0f && part.vol < 90.0f && part.bounding_radius < 1.35f)
                {
                    return true;
                }

                float hit_distance = 0.0f;
                if (check_ray_box(origin, dir, part, dist, &hit_distance))
                {
                    float ignore_threshold = 4.5f;
                    if (dist > 70.0f)
                        ignore_threshold = 9.0f;
                    else if (dist > 55.0f)
                        ignore_threshold = 7.0f;
                    else if (dist > 40.0f)
                        ignore_threshold = 6.0f;
                    else if (dist > 25.0f)
                        ignore_threshold = 5.0f;

                    bool near_origin_hit = (hit_distance < ignore_threshold && dist > 12.0f);
                    if (near_origin_hit && part.vol < 80.0f)
                        return true;
                    float part_dist = (part.pos - origin).magnitude();
                    float occlusion_gap = dist - hit_distance;
                    if (std::fabs(part_dist - dist) < 1.5f)
                    {
                        if (part.bounding_radius < 2.25f && part.vol < 120.0f)
                            return true;
                    }
                    if (occlusion_gap < 1.2f)
                    {
                        if (part.bounding_radius < 2.5f && part.vol < 150.0f)
                            return true;
                    }
                    return false;
                }
                return true;
            };

            if (use_candidates)
            {
                for (const WorldPart *part_ptr : candidate_parts)
                {
                    if (!test_part(*part_ptr))
                        return false;
                }
            }
            else
            {
                for (const auto &part : *local_geometry)
                {
                    if (!test_part(part))
                        return false;
                }
            }
            return true;
        };

        auto register_head_success = [&](float bonus_score, bool mark_lateral) {
            head_override = true;
            central_override = true;
            region_hits |= kRegionHead;
            head_score += bonus_score;
            visible_score = std::max(visible_score, std::max(required_visible, 1.0f));
            checked = std::max(checked, 1);
            if (mark_lateral)
                lateral_success = true;
        };

        bool direct_head_success = false;
        if (line_of_sight(from, head))
        {
            direct_head_success = true;
            register_head_success(2.5f, false);
        }

        if (!direct_head_success)
        {
            vector3 view_dir = head - from;
            float view_len = view_dir.magnitude();
            if (view_len < 1e-3f)
                view_dir = vector3(0.0f, 0.0f, -1.0f);
            else
                view_dir = view_dir / view_len;

            vector3 global_up(0.0f, 1.0f, 0.0f);
            vector3 right = view_dir.cross(global_up);
            float right_len = right.magnitude();
            if (right_len < 1e-3f)
                right = vector3(1.0f, 0.0f, 0.0f);
            else
                right = right / right_len;
            vector3 cam_up = right.cross(view_dir);
            float cam_up_len = cam_up.magnitude();
            if (cam_up_len < 1e-3f)
                cam_up = vector3(0.0f, 1.0f, 0.0f);
            else
                cam_up = cam_up / cam_up_len;

            std::array<vector3, 4> camera_offsets = {
                cam_up * 0.3f,
                cam_up * -0.3f,
                right * 0.3f,
                right * -0.3f};

            for (const auto &offset : camera_offsets)
            {
                vector3 origin = from + offset;
                if (line_of_sight(origin, head))
                {
                    direct_head_success = true;
                    register_head_success(1.7f, true);
                    break;
                }
            }
        }

        if (!direct_head_success)
        {
            for (const auto &ray : rays)
            {
                if (checked >= total_considered)
                    break;

                bool clear = is_ray_clear(ray.origin, ray.dir, ray.dist);
                if (clear)
                {
                    visible_score += ray.weight;
                    if (!ray.lateral)
                        central_score += ray.weight;
                    region_hits |= ray.mask;
                    if (ray.mask & kRegionHead)
                    {
                        head_score += ray.weight;
                        if (!ray.lateral && head_score >= 1.0f)
                            head_override = true;
                    }
                    if (ray.lateral)
                        lateral_success = true;

                    if (visible_score >= required_visible)
                    {
                        if (player_addr != 0)
                        {
                            std::lock_guard<std::mutex> vlk(vis_cache_mtx);
                            vis_cache[player_addr] = {true, std::chrono::steady_clock::now()};
                        }
                        return true;
                    }
                }

                checked++;
            }
        }

        float dynamic_required = std::max(1.0f, std::ceil(std::max(checked, 1) * base_ratio));
        dynamic_required = std::min(dynamic_required, max_visibility_goal);
        if (target_distance > 80.0f)
            dynamic_required = std::max(dynamic_required, 1.25f);
        if (target_distance > 120.0f)
            dynamic_required = std::max(dynamic_required, 1.5f);

        float central_gate = std::max(0.35f * dynamic_required, 0.55f);
        if (target_distance > 80.0f)
            central_gate = std::max(central_gate, 1.15f);
        if (target_distance > 120.0f)
            central_gate = std::max(central_gate, 1.4f);
        central_gate = std::min(central_gate, dynamic_required);
        auto recompute_central_satisfied = [&]() -> bool {
            return central_override || central_score >= central_gate;
        };
        bool central_satisfied = recompute_central_satisfied();

        bool body_region_visible = (region_hits & (kRegionHead | kRegionTorso | kRegionPelvis)) != 0;
        if (head_override)
            body_region_visible = true;
        if (body_region_visible && !central_satisfied && !head_override)
            body_region_visible = false;

        bool base_visible = visible_score >= dynamic_required;
        if (base_visible && !central_satisfied)
            base_visible = false;

        bool result = base_visible || body_region_visible;
        float confidence = (dynamic_required > 0.0f) ? std::clamp(visible_score / dynamic_required, 0.0f, 2.5f) : 0.0f;
        bool fallback_used = false;
        bool lateral_probe = false;

        float fallback_gate = std::max(0.35f * dynamic_required, 0.6f);
        if (target_distance > 80.0f)
            fallback_gate = std::max(fallback_gate, central_gate);
        bool fallback_allowed = ((visible_score >= fallback_gate) && central_satisfied) || head_override || direct_head_success;

        if (!result && fallback_allowed)
        {
            if (line_of_sight(from, head) || line_of_sight(from, torso) || line_of_sight(from, pelvis))
            {
                result = true;
                fallback_used = true;
                central_override = true;
                central_satisfied = true;
            }
        }

        if (!result && fallback_allowed)
        {
            for (const auto &offset : lateral_origins)
            {
                vector3 origin = from + offset;
                if (line_of_sight(origin, head))
                {
                    result = true;
                    fallback_used = true;
                    lateral_probe = true;
                    break;
                }
            }
        }

        bool final_result = result && (central_satisfied || head_override || direct_head_success);
        if (player_addr != 0)
        {
            auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> vlk(vis_cache_mtx);
            auto existing = vis_cache.find(player_addr);
            if (!final_result && existing != vis_cache.end() && existing->second.first)
            {
                auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - existing->second.second).count();
                constexpr int kVisibleHoldMs = 150;
                float cache_gate = std::max(0.25f * dynamic_required, 0.45f);
                bool allow_cache_hold = ((visible_score >= cache_gate) && central_satisfied) || head_override || direct_head_success;
                if (allow_cache_hold && age < kVisibleHoldMs)
                    final_result = true;
            }

            vis_cache[player_addr] = {final_result, now};
            if (vis_cache.size() > 100)
            {
                for (auto it = vis_cache.begin(); it != vis_cache.end();)
                {
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.second).count() > 1000)
                        it = vis_cache.erase(it);
                    else
                        ++it;
                }
            }
        }

        if (debug_enabled && player_addr != 0)
        {
            VisibilityDebug snapshot;
            snapshot.rays_enqueued = static_cast<int>(rays.size());
            snapshot.rays_checked = checked;
            snapshot.visible_score = visible_score;
            snapshot.required_score = dynamic_required;
            snapshot.height_delta = height_delta;
            snapshot.target_distance = target_distance;
            snapshot.head_score = head_score;
            snapshot.head_override = head_override;
            snapshot.fallback_used = fallback_used;
            snapshot.lateral_probe = lateral_probe || lateral_success;
            snapshot.candidate_parts = candidate_reference;
            snapshot.central_score = central_score;
            snapshot.central_required = central_gate;
            snapshot.confidence = confidence;
            snapshot.final_result = final_result;
            std::lock_guard<std::mutex> dlock(debug_snapshots_mtx);
            debug_snapshots[player_addr] = snapshot;
        }

        return final_result;
    }

    bool Wallcheck::line_of_sight(const vector3 &origin, const vector3 &target, float max_distance_override)
    {
        if (!ready)
            return true;

        auto local_geometry = std::atomic_load(&geometry_snapshot);
        if (!local_geometry || local_geometry->empty())
            return true;

        vector3 diff = target - origin;
        float dist = diff.magnitude();
        if (dist <= 0.05f)
            return true;
        vector3 dir = diff / dist;
        if (max_distance_override > 0.05f)
            dist = max_distance_override;

        vector3 seg_min(std::min(origin.X, target.X), std::min(origin.Y, target.Y), std::min(origin.Z, target.Z));
        vector3 seg_max(std::max(origin.X, target.X), std::max(origin.Y, target.Y), std::max(origin.Z, target.Z));
        float seg_margin = std::clamp(dist * 0.22f, 4.0f, 32.0f);
        vector3 expand(seg_margin, std::max(3.5f, seg_margin * 0.55f), seg_margin);
        vector3 query_min = seg_min - expand;
        vector3 query_max = seg_max + expand;

        for (const auto &part : *local_geometry)
        {
            if (!part.can_collide)
                continue;
            if (part.transparency >= 0.65f)
                continue;
            if (is_micro_occluder(part))
                continue;
            if (part.vol <= 0.0f || part.size.X <= 0.0f || part.size.Y <= 0.0f || part.size.Z <= 0.0f)
                continue;

            vector3 part_min = part.pos - part.los_half_extents;
            vector3 part_max = part.pos + part.los_half_extents;
            if (part_max.X < query_min.X || part_min.X > query_max.X ||
                part_max.Y < query_min.Y || part_min.Y > query_max.Y ||
                part_max.Z < query_min.Z || part_min.Z > query_max.Z)
                continue;

            float hit_distance = 0.0f;
            if (check_ray_box(origin, dir, part, dist, &hit_distance))
            {
                float ignore_threshold = 4.5f;
                if (dist > 70.0f)
                    ignore_threshold = 9.0f;
                else if (dist > 55.0f)
                    ignore_threshold = 7.0f;
                else if (dist > 40.0f)
                    ignore_threshold = 6.0f;
                else if (dist > 25.0f)
                    ignore_threshold = 5.0f;

                bool near_origin_hit = (hit_distance < ignore_threshold && dist > 12.0f);
                if (near_origin_hit && part.vol < 80.0f)
                    continue;
                if (part.vol < 15.0f)
                    continue;
                float part_dist = (part.pos - origin).magnitude();
                float occlusion_gap = dist - hit_distance;
                if (std::fabs(part_dist - dist) < 1.5f)
                {
                    if (part.bounding_radius < 2.25f && part.vol < 120.0f)
                        continue;
                }
                if (occlusion_gap < 1.2f)
                {
                    if (part.bounding_radius < 2.5f && part.vol < 150.0f)
                        continue;
                }
                return false;
            }
        }

        return true;
    }

    void Wallcheck::set_debug_enabled(bool enabled)
    {
        bool previous = debug_enabled.exchange(enabled);
        if (!enabled && previous)
        {
            std::lock_guard<std::mutex> lock(debug_snapshots_mtx);
            debug_snapshots.clear();
        }
    }

    bool Wallcheck::get_debug_snapshot(std::uint64_t player_addr, VisibilityDebug &out)
    {
        if (!debug_enabled || player_addr == 0)
            return false;

        std::lock_guard<std::mutex> lock(debug_snapshots_mtx);
        auto it = debug_snapshots.find(player_addr);
        if (it == debug_snapshots.end())
            return false;

        out = it->second;
        return true;
    }
}

