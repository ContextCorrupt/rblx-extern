#include "esp_module.hpp"
#include "util/renderer.hpp"
#include "util/engine/visualengine/visualengine.hpp"
#include "util/engine/datamodel/datamodel.hpp"
#include "util/engine/wallcheck/wallcheck.hpp"
#include "util/engine/offsets.hpp"
#include "util/profiling/profiler.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <unordered_map>
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
    constexpr auto kMaxCacheAge = std::chrono::milliseconds(650);
    constexpr auto kForgetInterval = std::chrono::seconds(4);
    constexpr auto kProfileFlushInterval = std::chrono::milliseconds(1500);
    constexpr auto kProfileIdleInterval = std::chrono::milliseconds(3000);
    constexpr auto kHealthRefreshInterval = std::chrono::milliseconds(175);

    struct PlayerRenderEntry
    {
        std::size_t index;
        vector3 hrp_pos;
        float distance_sq;
    };

    struct PartGeometry
    {
        vector3 position;
        matrix3 rotation;
        vector3 half;
        bool valid{false};
    };

    enum class SkeletonPoint : std::size_t
    {
        Head = 0,
        Neck,
        Collar,
        Chest,
        Pelvis,
        LeftShoulder,
        LeftElbow,
        LeftHand,
        RightShoulder,
        RightElbow,
        RightHand,
        LeftHip,
        LeftKnee,
        LeftFoot,
        RightHip,
        RightKnee,
        RightFoot,
        Count
    };

    struct CachedSkeleton
    {
        bool world_valid = false;
        std::array<vector3, static_cast<std::size_t>(SkeletonPoint::Count)> world_points{};
        std::array<bool, static_cast<std::size_t>(SkeletonPoint::Count)> point_valid{};
        std::chrono::steady_clock::time_point last_update{};
    };

    struct CachedVisual
    {
        bool valid = false;
        bool visible = false;
        bool head_valid = false;
        ImVec2 min{0.0f, 0.0f};
        ImVec2 max{0.0f, 0.0f};
        ImVec2 head{0.0f, 0.0f};
        vector3 head_world{};
        vector3 chest_world{};
        vector3 pelvis_world{};
    vector3 hrp_center{};
    vector3 hrp_half{};
    matrix3 hrp_rotation{};
    bool hrp_bounds_valid = false;
        std::array<vector3, 32> points{};
        std::size_t point_count = 0;
        float cached_health = 100.0f;
        float cached_max_health = 100.0f;
        std::chrono::steady_clock::time_point last_health_sample{};
        std::chrono::steady_clock::time_point last_update{};
        std::chrono::steady_clock::time_point last_seen{};
        CachedSkeleton skeleton;
    };

    std::unordered_map<std::uint64_t, CachedVisual> g_cached_visuals;

    struct BoneNode
    {
        vector3 world{};
        ImVec2 screen{0.0f, 0.0f};
        bool world_valid = false;
        bool screen_valid = false;
    };

    using BoneNodeArray = std::array<BoneNode, static_cast<std::size_t>(SkeletonPoint::Count)>;

    static BoneNode &get_bone(BoneNodeArray &nodes, SkeletonPoint point)
    {
        return nodes[static_cast<std::size_t>(point)];
    }

    static void reset_bones(BoneNodeArray &nodes)
    {
        for (auto &node : nodes)
            node = BoneNode{};
    }

    static void cache_bone_world(const BoneNodeArray &nodes, CachedSkeleton &cache)
    {
        cache.world_valid = false;
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            cache.world_points[i] = nodes[i].world;
            cache.point_valid[i] = nodes[i].world_valid;
            cache.world_valid = cache.world_valid || nodes[i].world_valid;
        }
    }

    static void restore_bone_world(const CachedSkeleton &cache, BoneNodeArray &nodes)
    {
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            nodes[i].world = cache.world_points[i];
            nodes[i].world_valid = cache.point_valid[i];
            nodes[i].screen_valid = false;
        }
    }

    double s_profile_refresh_ms = 0.0;
    double s_profile_attempts = 0.0;
    double s_profile_successes = 0.0;
    std::chrono::steady_clock::time_point s_profile_last_flush = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point s_profile_last_nonzero{};

    engine::Instance resolve_part(const engine::Player &player, const engine::Instance &primary, std::initializer_list<const char *> fallback_names)
    {
        if (primary.is_valid())
            return primary;

        engine::Instance character = player.character;
        if (!character.is_valid())
            return {};

        for (const char *name : fallback_names)
        {
            if (!name)
                continue;
            engine::Instance candidate = character.find_first_child(name);
            if (candidate.is_valid())
                return candidate;
        }
        return {};
    }

    bool capture_geometry(const engine::Instance &part, PartGeometry &geom)
    {
        geom.valid = false;
        if (!part.is_valid())
            return false;
        std::uint64_t primitive = cradle::memory::read<std::uint64_t>(part.address + Offsets::BasePart::Primitive);
        if (primitive < 0x10000)
            return false;
        vector3 size = cradle::memory::read<vector3>(primitive + Offsets::BasePart::Size);
        if (size.X <= 0.0f || size.Y <= 0.0f || size.Z <= 0.0f)
            return false;
        cframe cf = part.get_cframe();
        geom.position = cf.position;
        geom.rotation = cf.rotation;
        geom.half = size * 0.5f;
        geom.valid = true;
        return true;
    }

    bool sample_from_geometry(const PartGeometry &geom, const vector3 &normalized, vector3 &out)
    {
        if (!geom.valid)
            return false;
        vector3 clamped(
            std::clamp(normalized.X, -1.0f, 1.0f),
            std::clamp(normalized.Y, -1.0f, 1.0f),
            std::clamp(normalized.Z, -1.0f, 1.0f));
        vector3 scaled(clamped.X * geom.half.X, clamped.Y * geom.half.Y, clamped.Z * geom.half.Z);
        vector3 offset = geom.rotation.multiply(scaled);
        out = geom.position + offset;
        return true;
    }

    bool build_fallback_bounds(const CachedVisual &cached,
                               const vector3 &hrp_pos,
                               const matrix4 &view_matrix,
                               const vector2 &screen_size,
                               float client_offset_x,
                               float client_offset_y,
                               ImVec2 &out_min,
                               ImVec2 &out_max)
    {
        std::array<vector3, 5> anchors = {
            cached.head_world,
            cached.chest_world,
            cached.pelvis_world,
            hrp_pos + vector3(0.0f, 2.0f, 0.0f),
            hrp_pos - vector3(0.0f, 1.5f, 0.0f)};

        ImVec2 min_point(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
        ImVec2 max_point(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest());
        bool any = false;

        for (const auto &anchor : anchors)
        {
            if (!std::isfinite(anchor.X) || !std::isfinite(anchor.Y) || !std::isfinite(anchor.Z))
                continue;

            vector2 screen = world_to_screen(anchor, view_matrix, screen_size);
            if (screen.X == -1 || screen.Y == -1)
                continue;

            ImVec2 adjusted(screen.X + client_offset_x, screen.Y + client_offset_y);
            min_point.x = std::min(min_point.x, adjusted.x);
            min_point.y = std::min(min_point.y, adjusted.y);
            max_point.x = std::max(max_point.x, adjusted.x);
            max_point.y = std::max(max_point.y, adjusted.y);
            any = true;
        }

        if (!any)
            return false;

        float height = max_point.y - min_point.y;
        if (height < 2.0f)
            height = 2.0f;
        float width = std::max(height * 0.45f, 6.0f);
        float center_x = (min_point.x + max_point.x) * 0.5f;
        min_point.x = center_x - width * 0.5f;
        max_point.x = center_x + width * 0.5f;

        out_min = min_point;
        out_max = max_point;
        return true;
    }

    void refresh_health_cache(const engine::Player &player,
                              CachedVisual &cached,
                              std::chrono::steady_clock::time_point now,
                              bool force_sample = false)
    {
        if (!force_sample && cached.last_health_sample != std::chrono::steady_clock::time_point{} &&
            (now - cached.last_health_sample) < kHealthRefreshInterval)
            return;

        float health = player.health;
        float max_health = player.max_health;

        if (player.humanoid.is_valid())
        {
            float fetched = player.humanoid.get_health();
            if (std::isfinite(fetched))
                health = fetched;

            float fetched_max = player.humanoid.get_max_health();
            if (std::isfinite(fetched_max))
                max_health = fetched_max;
        }

        if (max_health <= 0.0f)
            max_health = 100.0f;

        cached.cached_health = health;
        cached.cached_max_health = max_health;
        cached.last_health_sample = now;
    }

    bool compute_cached_visual(const engine::Player &player,
                               const vector3 &hrp_pos,
                               const vector3 &camera_pos,
                               const matrix4 &view_matrix,
                               const vector2 &screen_size,
                               float client_offset_x,
                               float client_offset_y,
                               bool allow_wallcheck,
                               CachedVisual &cached)
    {
        (void)view_matrix;
        (void)screen_size;
        (void)client_offset_x;
        (void)client_offset_y;

        cached.point_count = 0;

        auto push_point = [&](const vector3 &pt) {
            if (!std::isfinite(pt.X) || !std::isfinite(pt.Y) || !std::isfinite(pt.Z))
                return;
            if (cached.point_count < cached.points.size())
                cached.points[cached.point_count++] = pt;
        };

        auto push_sample = [&](const PartGeometry &geom, const vector3 &offset) {
            vector3 pt;
            if (sample_from_geometry(geom, offset, pt))
                push_point(pt);
        };

        auto head_part = resolve_part(player, player.head, {"Head"});
        auto chest_part = resolve_part(player, player.upper_torso, {"UpperTorso", "Torso", "HumanoidRootPart"});
        auto pelvis_part = resolve_part(player, player.lower_torso, {"LowerTorso", "Torso", "HumanoidRootPart"});
        auto hrp_part = resolve_part(player, player.hrp,
                                     player.rig_type == 1 ? std::initializer_list<const char *>{"HumanoidRootPart", "Hitbox", "UpperTorso"}
                                                          : std::initializer_list<const char *>{"Torso", "HumanoidRootPart", "UpperTorso"});

        PartGeometry head_geom, chest_geom, pelvis_geom, hrp_geom;
        capture_geometry(head_part, head_geom);
        capture_geometry(chest_part, chest_geom);
        capture_geometry(pelvis_part, pelvis_geom);
        capture_geometry(hrp_part, hrp_geom);

        if (hrp_geom.valid)
        {
            cached.hrp_bounds_valid = true;
            cached.hrp_center = hrp_geom.position;
            cached.hrp_rotation = hrp_geom.rotation;
            cached.hrp_half = hrp_geom.half;
        }
        else
        {
            cached.hrp_bounds_valid = false;
        }

        vector3 head_center = head_geom.valid ? head_geom.position : (head_part.is_valid() ? head_part.get_pos() : hrp_pos + vector3(0.0f, 2.3f, 0.0f));
        vector3 chest_center = chest_geom.valid ? chest_geom.position : (chest_part.is_valid() ? chest_part.get_pos() : hrp_pos);
        vector3 pelvis_center = pelvis_geom.valid ? pelvis_geom.position : (pelvis_part.is_valid() ? pelvis_part.get_pos() : hrp_pos - vector3(0.0f, 1.0f, 0.0f));

        cached.head_world = head_center;
        cached.chest_world = chest_center;
        cached.pelvis_world = pelvis_center;
        cached.head_valid = head_part.is_valid() || head_geom.valid;

        push_point(head_center);
        push_point(chest_center);
        push_point(pelvis_center);

    push_sample(head_geom, vector3(0.0f, 1.0f, 0.0f));
    push_sample(chest_geom, vector3(1.0f, 0.2f, 0.0f));
    push_sample(chest_geom, vector3(-1.0f, 0.2f, 0.0f));
    push_sample(pelvis_geom, vector3(0.85f, -1.0f, 0.0f));
    push_sample(pelvis_geom, vector3(-0.85f, -1.0f, 0.0f));

        auto left_hand = resolve_part(player, player.left_hand, {"LeftHand", "LeftLowerArm", "Left Arm"});
        auto right_hand = resolve_part(player, player.right_hand, {"RightHand", "RightLowerArm", "Right Arm"});
        auto left_foot = resolve_part(player, player.left_foot, {"LeftFoot", "LeftLowerLeg", "Left Leg"});
        auto right_foot = resolve_part(player, player.right_foot, {"RightFoot", "RightLowerLeg", "Right Leg"});
        PartGeometry left_hand_geom, right_hand_geom, left_foot_geom, right_foot_geom;
        capture_geometry(left_hand, left_hand_geom);
        capture_geometry(right_hand, right_hand_geom);
        capture_geometry(left_foot, left_foot_geom);
        capture_geometry(right_foot, right_foot_geom);

        push_sample(left_hand_geom, vector3(0.0f, -1.0f, 0.0f));
        push_sample(right_hand_geom, vector3(0.0f, -1.0f, 0.0f));
        push_sample(left_foot_geom, vector3(0.0f, -1.0f, 0.0f));
        push_sample(right_foot_geom, vector3(0.0f, -1.0f, 0.0f));

        if (cached.hrp_bounds_valid)
        {
            matrix3 inv_rot = cached.hrp_rotation.transpose();
            vector3 min_local(-cached.hrp_half.X, -cached.hrp_half.Y, -cached.hrp_half.Z);
            vector3 max_local(cached.hrp_half.X, cached.hrp_half.Y, cached.hrp_half.Z);

            auto include_point = [&](const vector3 &pt) {
                if (!std::isfinite(pt.X) || !std::isfinite(pt.Y) || !std::isfinite(pt.Z))
                    return;
                vector3 local = inv_rot.multiply(pt - cached.hrp_center);
                min_local.X = std::min(min_local.X, local.X);
                min_local.Y = std::min(min_local.Y, local.Y);
                min_local.Z = std::min(min_local.Z, local.Z);
                max_local.X = std::max(max_local.X, local.X);
                max_local.Y = std::max(max_local.Y, local.Y);
                max_local.Z = std::max(max_local.Z, local.Z);
            };

            for (std::size_t i = 0; i < cached.point_count; ++i)
                include_point(cached.points[i]);

            auto include_geometry_extremes = [&](const PartGeometry &geom) {
                if (!geom.valid)
                    return;
                static const vector3 kOffsets[] = {
                    {0.0f, 1.0f, 0.0f},
                    {0.0f, -1.0f, 0.0f},
                    {1.0f, 0.0f, 0.0f},
                    {-1.0f, 0.0f, 0.0f},
                    {0.0f, 0.0f, 1.0f},
                    {0.0f, 0.0f, -1.0f}
                };
                for (const auto &offset : kOffsets)
                {
                    vector3 extreme;
                    if (sample_from_geometry(geom, offset, extreme))
                        include_point(extreme);
                }
            };

            include_geometry_extremes(head_geom);
            include_geometry_extremes(chest_geom);
            include_geometry_extremes(pelvis_geom);
            include_geometry_extremes(left_hand_geom);
            include_geometry_extremes(right_hand_geom);
            include_geometry_extremes(left_foot_geom);
            include_geometry_extremes(right_foot_geom);

            vector3 new_center_local(
                (min_local.X + max_local.X) * 0.5f,
                (min_local.Y + max_local.Y) * 0.5f,
                (min_local.Z + max_local.Z) * 0.5f);

            vector3 new_half(
                std::max(0.1f, (max_local.X - min_local.X) * 0.5f),
                std::max(0.1f, (max_local.Y - min_local.Y) * 0.5f),
                std::max(0.1f, (max_local.Z - min_local.Z) * 0.5f));

            cached.hrp_center = cached.hrp_center + cached.hrp_rotation.multiply(new_center_local);
            cached.hrp_half = new_half;
        }

        if (cached.point_count < 3)
        {
            push_point(hrp_pos + vector3(0.0f, 2.0f, 0.0f));
            push_point(hrp_pos);
            push_point(hrp_pos - vector3(0.0f, 2.0f, 0.0f));
        }

        cached.visible = !allow_wallcheck || engine::Wallcheck::is_visible(camera_pos, cached.head_world, cached.chest_world, cached.pelvis_world, player.character.address);
        cached.last_update = std::chrono::steady_clock::now();
        refresh_health_cache(player, cached, cached.last_update, true);
        cached.valid = cached.point_count > 0;
        return cached.valid;
    }

    bool reproject_geometry(const engine::Player &player,
                            const vector3 &hrp_pos,
                            const vector3 &camera_pos,
                            const matrix4 &view_matrix,
                            const vector2 &screen_size,
                            float client_offset_x,
                            float client_offset_y,
                            CachedVisual &cached)
    {
        (void)player;
        (void)hrp_pos;
        (void)camera_pos;

        if (!cached.valid || cached.point_count == 0)
            return false;

        ImVec2 min_point(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
        ImVec2 max_point(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest());
        bool any = false;

        for (std::size_t i = 0; i < cached.point_count; ++i)
        {
            vector2 screen = world_to_screen(cached.points[i], view_matrix, screen_size);
            if (screen.X == -1 || screen.Y == -1)
                continue;
            ImVec2 adjusted(screen.X + client_offset_x, screen.Y + client_offset_y);
            min_point.x = std::min(min_point.x, adjusted.x);
            min_point.y = std::min(min_point.y, adjusted.y);
            max_point.x = std::max(max_point.x, adjusted.x);
            max_point.y = std::max(max_point.y, adjusted.y);
            any = true;
        }

        if (!any)
            return false;

        cached.min = min_point;
        cached.max = max_point;

        cached.head_valid = false;
        vector2 head_screen = world_to_screen(cached.head_world, view_matrix, screen_size);
        if (!(head_screen.X == -1 || head_screen.Y == -1))
        {
            cached.head = ImVec2(head_screen.X + client_offset_x, head_screen.Y + client_offset_y);
            cached.head_valid = true;
        }

        return true;
    }

    bool build_skeleton_world(const engine::Player &player, BoneNodeArray &nodes)
    {
        reset_bones(nodes);

        auto set_node_from_part = [&](BoneNode &node, const engine::Instance &inst, const PartGeometry &geom, const vector3 &offset) -> bool {
            vector3 point;
            if (sample_from_geometry(geom, offset, point))
            {
                node.world = point;
                node.world_valid = true;
                return true;
            }
            if (geom.valid)
            {
                node.world = geom.position;
                node.world_valid = true;
                return true;
            }
            if (inst.is_valid())
            {
                node.world = inst.get_pos();
                node.world_valid = true;
                return true;
            }
            return false;
        };

        auto head_part = resolve_part(player, player.head, {"Head"});
        auto chest_part = resolve_part(player, player.upper_torso, {"UpperTorso", "Torso", "HumanoidRootPart"});
        auto pelvis_part = resolve_part(player, player.lower_torso, {"LowerTorso", "Torso", "HumanoidRootPart"});
        auto hrp_part = resolve_part(player, player.hrp,
                                     player.rig_type == 1 ? std::initializer_list<const char *>{"HumanoidRootPart", "Hitbox", "UpperTorso"}
                                                          : std::initializer_list<const char *>{"Torso", "HumanoidRootPart", "UpperTorso"});

        if (!chest_part.is_valid())
            chest_part = hrp_part;
        if (!pelvis_part.is_valid())
            pelvis_part = hrp_part;

        PartGeometry head_geom, chest_geom, pelvis_geom, hrp_geom;
        capture_geometry(head_part, head_geom);
        capture_geometry(chest_part, chest_geom);
        capture_geometry(pelvis_part, pelvis_geom);
        capture_geometry(hrp_part, hrp_geom);

        auto &head_node = get_bone(nodes, SkeletonPoint::Head);
        set_node_from_part(head_node, head_part, head_geom, vector3(0.0f, 0.0f, 0.0f));

        auto &neck_node = get_bone(nodes, SkeletonPoint::Neck);
        if (!set_node_from_part(neck_node, chest_part, chest_geom, vector3(0.0f, 0.9f, 0.0f)))
        {
            if (head_node.world_valid)
                set_node_from_part(neck_node, head_part, head_geom, vector3(0.0f, -0.8f, 0.0f));
        }

        auto &chest_node = get_bone(nodes, SkeletonPoint::Chest);
        if (!set_node_from_part(chest_node, chest_part, chest_geom, vector3(0.0f, 0.1f, 0.0f)) && hrp_geom.valid)
            set_node_from_part(chest_node, hrp_part, hrp_geom, vector3(0.0f, 0.1f, 0.0f));

        auto &pelvis_node = get_bone(nodes, SkeletonPoint::Pelvis);
        if (!set_node_from_part(pelvis_node, pelvis_part, pelvis_geom, vector3(0.0f, -0.2f, 0.0f)))
            pelvis_node = chest_node;

        if (!neck_node.world_valid && head_node.world_valid && chest_node.world_valid)
        {
            neck_node.world = chest_node.world + (head_node.world - chest_node.world) * 0.35f;
            neck_node.world_valid = true;
        }

        auto &collar_node = get_bone(nodes, SkeletonPoint::Collar);
        if (neck_node.world_valid)
        {
            collar_node = neck_node;
            collar_node.world.Y -= 0.08f;
            collar_node.world_valid = true;
        }
        else if (chest_node.world_valid)
        {
            collar_node = chest_node;
            collar_node.world_valid = true;
        }

        auto left_upper_arm = resolve_part(player, player.left_upper_arm, {"LeftUpperArm", "Left Arm"});
        auto right_upper_arm = resolve_part(player, player.right_upper_arm, {"RightUpperArm", "Right Arm"});
        auto left_lower_arm = resolve_part(player, player.left_lower_arm, {"LeftLowerArm", "Left Arm"});
        auto right_lower_arm = resolve_part(player, player.right_lower_arm, {"RightLowerArm", "Right Arm"});
        auto left_hand_part = resolve_part(player, player.left_hand, {"LeftHand", "Left Arm", "LeftLowerArm"});
        auto right_hand_part = resolve_part(player, player.right_hand, {"RightHand", "Right Arm", "RightLowerArm"});

        PartGeometry left_upper_arm_geom, right_upper_arm_geom, left_lower_arm_geom, right_lower_arm_geom, left_hand_geom, right_hand_geom;
        capture_geometry(left_upper_arm, left_upper_arm_geom);
        capture_geometry(right_upper_arm, right_upper_arm_geom);
        capture_geometry(left_lower_arm, left_lower_arm_geom);
        capture_geometry(right_lower_arm, right_lower_arm_geom);
        capture_geometry(left_hand_part, left_hand_geom);
        capture_geometry(right_hand_part, right_hand_geom);

        auto left_upper_leg = resolve_part(player, player.left_upper_leg, {"LeftUpperLeg", "Left Leg"});
        auto right_upper_leg = resolve_part(player, player.right_upper_leg, {"RightUpperLeg", "Right Leg"});
        auto left_lower_leg = resolve_part(player, player.left_lower_leg, {"LeftLowerLeg", "Left Leg"});
        auto right_lower_leg = resolve_part(player, player.right_lower_leg, {"RightLowerLeg", "Right Leg"});
        auto left_foot_part = resolve_part(player, player.left_foot, {"LeftFoot", "Left Leg", "LeftLowerLeg"});
        auto right_foot_part = resolve_part(player, player.right_foot, {"RightFoot", "Right Leg", "RightLowerLeg"});

        PartGeometry left_upper_leg_geom, right_upper_leg_geom, left_lower_leg_geom, right_lower_leg_geom, left_foot_geom, right_foot_geom;
        capture_geometry(left_upper_leg, left_upper_leg_geom);
        capture_geometry(right_upper_leg, right_upper_leg_geom);
        capture_geometry(left_lower_leg, left_lower_leg_geom);
        capture_geometry(right_lower_leg, right_lower_leg_geom);
        capture_geometry(left_foot_part, left_foot_geom);
        capture_geometry(right_foot_part, right_foot_geom);

        auto &left_shoulder = get_bone(nodes, SkeletonPoint::LeftShoulder);
        auto &right_shoulder = get_bone(nodes, SkeletonPoint::RightShoulder);
        if (!set_node_from_part(left_shoulder, left_upper_arm, left_upper_arm_geom, vector3(0.0f, 1.0f, 0.0f)))
            set_node_from_part(left_shoulder, chest_part, chest_geom, vector3(0.85f, 0.4f, 0.0f));
        if (!set_node_from_part(right_shoulder, right_upper_arm, right_upper_arm_geom, vector3(0.0f, 1.0f, 0.0f)))
            set_node_from_part(right_shoulder, chest_part, chest_geom, vector3(-0.85f, 0.4f, 0.0f));

        auto align_shoulder_height = [&](BoneNode &shoulder, float fallback_sign) {
            if (!shoulder.world_valid && collar_node.world_valid)
            {
                shoulder.world = collar_node.world + vector3(fallback_sign * 0.42f, 0.0f, 0.0f);
                shoulder.world_valid = true;
            }
            if (shoulder.world_valid && collar_node.world_valid)
                shoulder.world.Y = collar_node.world.Y;
            else if (shoulder.world_valid && neck_node.world_valid)
                shoulder.world.Y = neck_node.world.Y;
        };

        align_shoulder_height(left_shoulder, 1.0f);
        align_shoulder_height(right_shoulder, -1.0f);

        auto &left_elbow = get_bone(nodes, SkeletonPoint::LeftElbow);
        auto &right_elbow = get_bone(nodes, SkeletonPoint::RightElbow);
        auto &left_hand = get_bone(nodes, SkeletonPoint::LeftHand);
        auto &right_hand = get_bone(nodes, SkeletonPoint::RightHand);

        auto build_arm_chain = [&](const engine::Instance &upper_inst, const PartGeometry &upper_geom,
                                   const engine::Instance &lower_inst, const PartGeometry &lower_geom,
                                   const engine::Instance &hand_inst, const PartGeometry &hand_geom,
                                   BoneNode &shoulder, BoneNode &elbow, BoneNode &hand_node) {
            (void)upper_inst;
            (void)lower_inst;
            vector3 joint_point;
            if (!elbow.world_valid && sample_from_geometry(upper_geom, vector3(0.0f, -1.0f, 0.0f), joint_point))
            {
                elbow.world = joint_point;
                elbow.world_valid = true;
            }
            if (sample_from_geometry(lower_geom, vector3(0.0f, 1.0f, 0.0f), joint_point))
            {
                if (elbow.world_valid)
                    elbow.world = (elbow.world + joint_point) * 0.5f;
                else
                {
                    elbow.world = joint_point;
                    elbow.world_valid = true;
                }
            }
            if (sample_from_geometry(lower_geom, vector3(0.0f, -1.0f, 0.0f), joint_point))
            {
                hand_node.world = joint_point;
                hand_node.world_valid = true;
            }
            if (!hand_node.world_valid)
                set_node_from_part(hand_node, hand_inst, hand_geom, vector3(0.0f, -0.2f, 0.0f));
            if (!hand_node.world_valid && elbow.world_valid && shoulder.world_valid)
            {
                vector3 dir = elbow.world - shoulder.world;
                if (dir.magnitude() > 0.01f)
                {
                    hand_node.world = elbow.world + dir;
                    hand_node.world_valid = true;
                }
            }
        };

        build_arm_chain(left_upper_arm, left_upper_arm_geom, left_lower_arm, left_lower_arm_geom, left_hand_part, left_hand_geom, left_shoulder, left_elbow, left_hand);
        build_arm_chain(right_upper_arm, right_upper_arm_geom, right_lower_arm, right_lower_arm_geom, right_hand_part, right_hand_geom, right_shoulder, right_elbow, right_hand);

        auto &left_hip = get_bone(nodes, SkeletonPoint::LeftHip);
        auto &right_hip = get_bone(nodes, SkeletonPoint::RightHip);
        const engine::Instance &hip_anchor_inst = pelvis_part.is_valid() ? pelvis_part : chest_part;
        const PartGeometry &hip_anchor_geom = pelvis_geom.valid ? pelvis_geom : chest_geom;
        if (!set_node_from_part(left_hip, left_upper_leg, left_upper_leg_geom, vector3(0.0f, 1.0f, 0.0f)))
            set_node_from_part(left_hip, hip_anchor_inst, hip_anchor_geom, vector3(0.55f, -0.4f, 0.0f));
        if (!set_node_from_part(right_hip, right_upper_leg, right_upper_leg_geom, vector3(0.0f, 1.0f, 0.0f)))
            set_node_from_part(right_hip, hip_anchor_inst, hip_anchor_geom, vector3(-0.55f, -0.4f, 0.0f));

        auto &left_knee = get_bone(nodes, SkeletonPoint::LeftKnee);
        auto &right_knee = get_bone(nodes, SkeletonPoint::RightKnee);
        auto &left_foot = get_bone(nodes, SkeletonPoint::LeftFoot);
        auto &right_foot = get_bone(nodes, SkeletonPoint::RightFoot);

        auto build_leg_chain = [&](const engine::Instance &upper_inst, const PartGeometry &upper_geom,
                                   const engine::Instance &lower_inst, const PartGeometry &lower_geom,
                                   const engine::Instance &foot_inst, const PartGeometry &foot_geom,
                                   BoneNode &hip, BoneNode &knee, BoneNode &foot_node) {
            (void)upper_inst;
            (void)lower_inst;
            vector3 joint_point;
            if (!knee.world_valid && sample_from_geometry(upper_geom, vector3(0.0f, -1.0f, 0.0f), joint_point))
            {
                knee.world = joint_point;
                knee.world_valid = true;
            }
            if (sample_from_geometry(lower_geom, vector3(0.0f, 1.0f, 0.0f), joint_point))
            {
                if (knee.world_valid)
                    knee.world = (knee.world + joint_point) * 0.5f;
                else
                {
                    knee.world = joint_point;
                    knee.world_valid = true;
                }
            }
            if (sample_from_geometry(lower_geom, vector3(0.0f, -1.0f, 0.0f), joint_point))
            {
                foot_node.world = joint_point;
                foot_node.world_valid = true;
            }
            if (!foot_node.world_valid)
                set_node_from_part(foot_node, foot_inst, foot_geom, vector3(0.0f, -0.8f, 0.0f));
            if (!foot_node.world_valid && knee.world_valid && hip.world_valid)
            {
                vector3 dir = knee.world - hip.world;
                if (dir.magnitude() > 0.01f)
                {
                    foot_node.world = knee.world + dir;
                    foot_node.world_valid = true;
                }
            }
        };

        build_leg_chain(left_upper_leg, left_upper_leg_geom, left_lower_leg, left_lower_leg_geom, left_foot_part, left_foot_geom, left_hip, left_knee, left_foot);
        build_leg_chain(right_upper_leg, right_upper_leg_geom, right_lower_leg, right_lower_leg_geom, right_foot_part, right_foot_geom, right_hip, right_knee, right_foot);

        bool any = false;
        for (const auto &node : nodes)
        {
            if (node.world_valid)
            {
                any = true;
                break;
            }
        }
        return any;
    }
}

ESPModule::ESPModule()
    : Module("esp", "render player overlays")
{
    settings.push_back(Setting("enable esp", true));
    settings.push_back(Setting("esp box mode", 0, 0, 1));
    settings.push_back(Setting("names", true));
    settings.push_back(Setting("distance", true));
    settings.push_back(Setting("health bar", true));
    settings.push_back(Setting("head circle", true));
    settings.push_back(Setting("head circle color", 1.0f, 1.0f, 1.0f, 1.0f));
    settings.push_back(Setting("skeleton", false));
    settings.push_back(Setting("skeleton refresh ms", 150.0f, 50.0f, 1000.0f));
    settings.push_back(Setting("skeleton max refresh per frame", 3, 1, 16));
    settings.push_back(Setting("skeleton max distance", 600.0f, 0.0f, 5000.0f));
    settings.push_back(Setting("skeleton visible only", true));
    settings.push_back(Setting("skeleton max draws per frame", 10, 1, 64));
    settings.push_back(Setting("visible color", 0.0f, 1.0f, 0.0f, 1.0f));
    settings.push_back(Setting("hidden color", 1.0f, 0.0f, 0.0f, 1.0f));
    settings.push_back(Setting("wall check", true));
    settings.push_back(Setting("wallcheck debug overlay", false));
    settings.push_back(Setting("team check", false));
    settings.push_back(Setting("refresh ms", 60.0f, 16.0f, 250.0f));
    settings.push_back(Setting("refresh limit", 8, 1, 64));
    settings.push_back(Setting("esp max renders", 24, 1, 128));
    settings.push_back(Setting("max distance", 1200.0f, 0.0f, 5000.0f));
}

void ESPModule::on_render()
{
    auto now = std::chrono::steady_clock::now();

    auto enable_setting = get_setting("enable esp");
    auto box_mode_setting = get_setting("esp box mode");
    auto name_setting = get_setting("names");
    auto distance_setting = get_setting("distance");
    auto health_bar_setting = get_setting("health bar");
    auto head_circle_setting = get_setting("head circle");
    auto head_circle_color_setting = get_setting("head circle color");
    auto skeleton_setting = get_setting("skeleton");
    auto skeleton_refresh_ms_setting = get_setting("skeleton refresh ms");
    auto skeleton_refresh_budget_setting = get_setting("skeleton max refresh per frame");
    auto skeleton_max_distance_setting = get_setting("skeleton max distance");
    auto skeleton_visible_only_setting = get_setting("skeleton visible only");
    auto skeleton_draw_limit_setting = get_setting("skeleton max draws per frame");
    auto visible_color = get_setting("visible color");
    auto hidden_color = get_setting("hidden color");
    auto wallcheck_setting = get_setting("wall check");
    auto wallcheck_debug_setting = get_setting("wallcheck debug overlay");
    auto refresh_ms_setting = get_setting("refresh ms");
    auto refresh_limit_setting = get_setting("refresh limit");
    auto max_render_setting = get_setting("esp max renders");
    auto max_distance_setting = get_setting("max distance");
    auto team_check_setting = get_setting("team check");

    float max_distance_sq = std::numeric_limits<float>::max();
    if (max_distance_setting)
    {
        float max_dist = std::max(0.0f, max_distance_setting->value.float_val);
        max_distance_sq = max_dist * max_dist;
    }

    float refresh_ms = refresh_ms_setting ? refresh_ms_setting->value.float_val : 60.0f;
    refresh_ms = std::clamp(refresh_ms, 16.0f, 250.0f);
    auto refresh_interval = std::chrono::milliseconds(static_cast<int>(refresh_ms));

    int refresh_limit_val = refresh_limit_setting ? refresh_limit_setting->value.int_val : 8;
    refresh_limit_val = std::clamp(refresh_limit_val, 1, 64);
    std::size_t refresh_limit = static_cast<std::size_t>(refresh_limit_val);

    bool allow_wallcheck = wallcheck_setting ? wallcheck_setting->value.bool_val : true;
    bool wallcheck_debug_enabled = wallcheck_debug_setting && wallcheck_debug_setting->value.bool_val;
    engine::Wallcheck::set_debug_enabled(wallcheck_debug_enabled);

    int max_render_val = max_render_setting ? max_render_setting->value.int_val : 24;
    max_render_val = std::clamp(max_render_val, 1, 128);
    std::size_t max_render_count = static_cast<std::size_t>(max_render_val);

    float base_refresh_ms = static_cast<float>(refresh_interval.count());
    base_refresh_ms = std::max(base_refresh_ms, 1.0f);

    float skeleton_refresh_ms = skeleton_refresh_ms_setting ? skeleton_refresh_ms_setting->value.float_val : 150.0f;
    skeleton_refresh_ms = std::clamp(skeleton_refresh_ms, 50.0f, 1500.0f);

    std::size_t skeleton_refresh_budget = 3;
    if (skeleton_refresh_budget_setting)
        skeleton_refresh_budget = std::max(1, skeleton_refresh_budget_setting->value.int_val);

    float skeleton_max_distance = skeleton_max_distance_setting ? skeleton_max_distance_setting->value.float_val : 600.0f;
    skeleton_max_distance = std::max(0.0f, skeleton_max_distance);

    bool skeleton_visible_only = skeleton_visible_only_setting ? skeleton_visible_only_setting->value.bool_val : true;
    std::size_t skeleton_draw_limit = 10;
    if (skeleton_draw_limit_setting)
        skeleton_draw_limit = static_cast<std::size_t>(std::clamp(skeleton_draw_limit_setting->value.int_val, 1, 64));

    struct FrameProfiler
    {
        std::size_t attempts = 0;
        std::size_t successes = 0;
        std::chrono::nanoseconds refresh_time{0};
    };

    FrameProfiler frame_profiler;

    bool esp_enabled = !enable_setting || enable_setting->value.bool_val;
    if (!esp_enabled)
        return;

    int box_mode = 0;
    if (box_mode_setting && box_mode_setting->type == cradle::modules::SettingType::INT)
    {
        int min_val = box_mode_setting->range.int_range.min;
        int max_val = box_mode_setting->range.int_range.max;
        box_mode = std::clamp(box_mode_setting->value.int_val, min_val, max_val);
        if (box_mode_setting->value.int_val != box_mode)
            box_mode_setting->value.int_val = box_mode;
    }

    const bool draw_box = box_mode == 0;
    const bool draw_box3d = box_mode == 1;
    const bool draw_name = name_setting && name_setting->value.bool_val;
    const bool draw_distance = distance_setting && distance_setting->value.bool_val;
    const bool draw_health = health_bar_setting && health_bar_setting->value.bool_val;
    const bool draw_head_circle = head_circle_setting && head_circle_setting->value.bool_val;
    const bool draw_skeleton = skeleton_setting && skeleton_setting->value.bool_val;
    if (!(draw_box || draw_box3d || draw_name || draw_distance || draw_health || draw_head_circle || draw_skeleton))
        return;

    bool team_check = team_check_setting && team_check_setting->value.bool_val;

    auto players_snapshot = PlayerCache::get_players();
    if (!players_snapshot || players_snapshot->empty())
        return;
    const auto &players = *players_snapshot;

            using cradle::engine::VisualEngine;
            VisualEngine ve = VisualEngine::get_instance();
            if (!ve.is_valid())
                return;

            vector2 screen_size = ve.get_dimensions();
            matrix4 view_matrix = ve.get_viewmatrix();

            float client_offset_x = 0.0f;
            float client_offset_y = 0.0f;

            HWND roblox_window = FindWindowA(nullptr, "Roblox");
            if (roblox_window)
            {
                RECT client_rect;
                RECT window_rect;
                if (GetClientRect(roblox_window, &client_rect) && GetWindowRect(roblox_window, &window_rect))
                {
                    POINT client_origin{client_rect.left, client_rect.top};
                    ClientToScreen(roblox_window, &client_origin);
                    client_offset_x = static_cast<float>(client_origin.x - window_rect.left);
                    client_offset_y = static_cast<float>(client_origin.y - window_rect.top);
                    screen_size.X = static_cast<float>(client_rect.right - client_rect.left);
                    screen_size.Y = static_cast<float>(client_rect.bottom - client_rect.top);
                }
            }

            if (screen_size.X <= 0 || screen_size.Y <= 0)
                return;

            DataModel dm = DataModel::get_instance();
            if (!dm.is_valid())
                return;

            Instance camera = dm.get_current_camera();
            if (!camera.is_valid())
                return;

            vector3 camera_pos = camera.get_pos();

            ImGuiViewport *viewport = ImGui::GetMainViewport();
            if (!viewport)
                return;

            ImDrawList *draw_list = ImGui::GetBackgroundDrawList();
            if (!draw_list)
                return;

            auto local = PlayerCache::get_local_player();
            std::string local_team = local.team;
            auto scale_color = [](ImU32 color) -> ImU32 { return color; };
            std::size_t refresh_count = 0;

            std::vector<PlayerRenderEntry> ordered_players;
            ordered_players.reserve(players.size());

            std::size_t snapshot_players = players.size();
            std::size_t skeleton_refreshes_this_frame = 0;
            std::size_t skeleton_draws_this_frame = 0;

            for (std::size_t idx = 0; idx < players.size(); ++idx)
            {
                auto &player = players[idx];
                auto head_inst = resolve_part(player, player.head, {"Head"});
                auto hrp_inst = resolve_part(player, player.hrp, player.rig_type == 1 ? std::initializer_list<const char *>{"HumanoidRootPart", "Hitbox", "UpperTorso"} : std::initializer_list<const char *>{"Torso", "HumanoidRootPart", "UpperTorso"});
                if (!head_inst.is_valid() || !hrp_inst.is_valid())
                    continue;

                if (local.is_valid())
                {
                    if (player.character.address == local.character.address)
                        continue;
                    if (player.hrp.address == local.hrp.address)
                        continue;
                }

                if (team_check && !player.team.empty() && !local_team.empty() && player.team == local_team)
                    continue;

                vector3 hrp_pos = hrp_inst.get_pos();
                vector3 d = hrp_pos - camera_pos;
                float dist_sq = d.X * d.X + d.Y * d.Y + d.Z * d.Z;
                if (dist_sq > max_distance_sq)
                    continue;

                ordered_players.push_back(PlayerRenderEntry{idx, hrp_pos, dist_sq});
            }

            std::sort(ordered_players.begin(), ordered_players.end(), [](const PlayerRenderEntry &a, const PlayerRenderEntry &b) {
                return a.distance_sq < b.distance_sq;
            });

            std::size_t eligible_players = ordered_players.size();

            if (ordered_players.size() < max_render_count)
                max_render_count = ordered_players.size();

            draw_list->PushClipRectFullScreen();
            int rendered_players = 0;
            std::size_t fallback_attempts = 0;
            std::size_t fallback_success = 0;

            for (const auto &entry : ordered_players)
            {
                if (static_cast<std::size_t>(rendered_players) >= max_render_count)
                    break;

                auto &player = players[entry.index];
                vector3 hrp_pos = entry.hrp_pos;
                float dist_sq = entry.distance_sq;
                float distance = std::sqrt(std::max(dist_sq, 0.0f));

                const bool need_distance_text = draw_distance;

                float distance_factor = 1.0f + std::min(distance / 600.0f, 2.0f);
                distance_factor = std::clamp(distance_factor, 1.0f, 3.0f);
                int player_refresh_ms = static_cast<int>(base_refresh_ms * distance_factor);
                player_refresh_ms = std::clamp(player_refresh_ms, static_cast<int>(base_refresh_ms), 600);
                auto player_refresh_interval = std::chrono::milliseconds(player_refresh_ms);

                auto &cached = g_cached_visuals[player.character.address];
                cached.last_seen = now;
                bool needs_refresh = !cached.valid;
                bool stale = !needs_refresh && (now - cached.last_update) > player_refresh_interval;
                bool aged_out = !needs_refresh && (now - cached.last_update) > kMaxCacheAge;

                bool require_refresh = needs_refresh || stale || aged_out;

                if (require_refresh && (refresh_count < refresh_limit || needs_refresh || aged_out))
                {
                    auto compute_start = std::chrono::high_resolution_clock::now();

                    bool computed = compute_cached_visual(player, hrp_pos, camera_pos, view_matrix, screen_size, client_offset_x, client_offset_y, allow_wallcheck, cached);

                    if (computed)
                    {
                        cached.last_update = now;
                        cached.valid = true;
                    }
                    else
                    {
                        cached.valid = false;
                    }

                    auto compute_end = std::chrono::high_resolution_clock::now();
                    frame_profiler.attempts++;
                    frame_profiler.refresh_time += std::chrono::duration_cast<std::chrono::nanoseconds>(compute_end - compute_start);
                    if (computed)
                        frame_profiler.successes++;

                    refresh_count++;
                }

                if (!cached.valid)
                    continue;

                if ((now - cached.last_update) > kMaxCacheAge)
                {
                    cached.valid = false;
                    continue;
                }

                if ((now - cached.last_seen) > kForgetInterval)
                {
                    cached.valid = false;
                    continue;
                }

                bool reprojected = reproject_geometry(player, hrp_pos, camera_pos, view_matrix, screen_size, client_offset_x, client_offset_y, cached);
                if (!reprojected)
                {
                    ++fallback_attempts;
                    ImVec2 fb_min, fb_max;
                    if (build_fallback_bounds(cached, hrp_pos, view_matrix, screen_size, client_offset_x, client_offset_y, fb_min, fb_max))
                    {
                        cached.min = fb_min;
                        cached.max = fb_max;
                        vector2 head_screen = world_to_screen(cached.head_world, view_matrix, screen_size);
                        if (!(head_screen.X == -1 || head_screen.Y == -1))
                        {
                            cached.head = ImVec2(head_screen.X + client_offset_x, head_screen.Y + client_offset_y);
                            cached.head_valid = true;
                        }
                        else
                        {
                            cached.head_valid = false;
                        }
                        ++fallback_success;
                    }
                    else
                    {
                        cached.valid = false;
                        continue;
                    }
                }

                const float raw_height = cached.max.y - cached.min.y;
                if (raw_height < 2.0f)
                    continue;

                refresh_health_cache(player, cached, now, false);
                float current_health = cached.cached_health;
                float current_max = cached.cached_max_health;
                if (current_max <= 0.0f)
                    current_max = 100.0f;

                if (current_health <= 0.0f)
                    continue;

                bool rendered_this_player = false;

                auto compute_state_color = [&](bool is_visible) -> ImU32 {
                    if (is_visible && visible_color)
                    {
                        return IM_COL32(
                            static_cast<int>(visible_color->value.color_val[0] * 255),
                            static_cast<int>(visible_color->value.color_val[1] * 255),
                            static_cast<int>(visible_color->value.color_val[2] * 255),
                            static_cast<int>(visible_color->value.color_val[3] * 255));
                    }
                    if (!is_visible && hidden_color)
                    {
                        return IM_COL32(
                            static_cast<int>(hidden_color->value.color_val[0] * 255),
                            static_cast<int>(hidden_color->value.color_val[1] * 255),
                            static_cast<int>(hidden_color->value.color_val[2] * 255),
                            static_cast<int>(hidden_color->value.color_val[3] * 255));
                    }
                    return is_visible ? IM_COL32(0, 255, 0, 255) : IM_COL32(255, 0, 0, 255);
                };

                ImU32 state_color = compute_state_color(cached.visible);

                if (draw_box)
                {
                    float thickness = std::clamp(raw_height * 0.0025f + 1.0f, 1.0f, 3.0f);
                    draw_list->AddRect(cached.min, cached.max, scale_color(state_color), 2.0f, 0, thickness);
                    rendered_this_player = true;
                }

                if (draw_box3d && cached.hrp_bounds_valid)
                {
                    constexpr int kCornerSigns[8][3] = {
                        {-1, -1, -1},
                        {1, -1, -1},
                        {1, 1, -1},
                        {-1, 1, -1},
                        {-1, -1, 1},
                        {1, -1, 1},
                        {1, 1, 1},
                        {-1, 1, 1}};

                    std::array<ImVec2, 8> projected{};
                    std::array<bool, 8> projected_valid{};

                    for (int i = 0; i < 8; ++i)
                    {
                        vector3 corner_local(
                            cached.hrp_half.X * static_cast<float>(kCornerSigns[i][0]),
                            cached.hrp_half.Y * static_cast<float>(kCornerSigns[i][1]),
                            cached.hrp_half.Z * static_cast<float>(kCornerSigns[i][2]));
                        vector3 rotated = cached.hrp_rotation.multiply(corner_local);
                        vector3 world = cached.hrp_center + rotated;
                        vector2 screen = world_to_screen(world, view_matrix, screen_size);
                        if (screen.X == -1 || screen.Y == -1)
                        {
                            projected_valid[i] = false;
                            continue;
                        }
                        projected[i] = ImVec2(screen.X + client_offset_x, screen.Y + client_offset_y);
                        projected_valid[i] = true;
                    }

                    const float box3d_thickness = std::clamp(raw_height * 0.0020f + 0.8f, 0.75f, 2.5f);
                    auto draw_edge = [&](int a, int b) {
                        if (!projected_valid[a] || !projected_valid[b])
                            return;
                        draw_list->AddLine(projected[a], projected[b], scale_color(state_color), box3d_thickness);
                    };

                    draw_edge(0, 1);
                    draw_edge(1, 2);
                    draw_edge(2, 3);
                    draw_edge(3, 0);
                    draw_edge(4, 5);
                    draw_edge(5, 6);
                    draw_edge(6, 7);
                    draw_edge(7, 4);
                    draw_edge(0, 4);
                    draw_edge(1, 5);
                    draw_edge(2, 6);
                    draw_edge(3, 7);

                    rendered_this_player = true;
                }

                if (draw_skeleton)
                {
                    constexpr float kSkeletonMinHeight = 14.0f;
                    bool allow_skeleton = raw_height >= kSkeletonMinHeight;
                    if (allow_skeleton && skeleton_max_distance > 0.0f && distance > skeleton_max_distance)
                        allow_skeleton = false;
                    if (allow_skeleton && skeleton_visible_only && !cached.visible)
                        allow_skeleton = false;
                    if (allow_skeleton && skeleton_draw_limit > 0 && skeleton_draws_this_frame >= skeleton_draw_limit)
                        allow_skeleton = false;

                    if (allow_skeleton)
                    {
                        float distance_scale = 1.0f + std::min(distance / 600.0f, 3.5f);
                        int interval_ms = static_cast<int>(std::clamp(skeleton_refresh_ms * distance_scale, 50.0f, 2000.0f));
                        auto skeleton_interval = std::chrono::milliseconds(interval_ms);
                        if (skeleton_interval < player_refresh_interval)
                            skeleton_interval = player_refresh_interval;

                        bool need_skeleton_refresh = !cached.skeleton.world_valid ||
                                                     (now - cached.skeleton.last_update) > skeleton_interval;

                        BoneNodeArray bones;
                        bool have_skeleton = false;

                        if (need_skeleton_refresh)
                        {
                            if (skeleton_refreshes_this_frame < skeleton_refresh_budget)
                            {
                                if (build_skeleton_world(player, bones))
                                {
                                    cache_bone_world(bones, cached.skeleton);
                                    cached.skeleton.world_valid = true;
                                    cached.skeleton.last_update = now;
                                    have_skeleton = true;
                                }
                                else
                                {
                                    cached.skeleton.world_valid = false;
                                }
                                ++skeleton_refreshes_this_frame;
                            }
                            else if (cached.skeleton.world_valid)
                            {
                                restore_bone_world(cached.skeleton, bones);
                                have_skeleton = true;
                            }
                        }
                        else if (cached.skeleton.world_valid)
                        {
                            restore_bone_world(cached.skeleton, bones);
                            have_skeleton = true;
                        }

                        if (have_skeleton)
                        {
                            auto project_point = [&](BoneNode &node) {
                                if (!node.world_valid)
                                    return;
                                vector2 screen = world_to_screen(node.world, view_matrix, screen_size);
                                if (screen.X == -1 || screen.Y == -1)
                                    return;
                                node.screen = ImVec2(screen.X + client_offset_x, screen.Y + client_offset_y);
                                node.screen_valid = true;
                            };

                            for (auto &node : bones)
                                project_point(node);

                            bool skeleton_drawn = false;
                            const float skeleton_thickness = std::clamp(raw_height * 0.0022f + 0.8f, 0.75f, 2.8f);

                            std::array<ImVec2, 8> polyline_buffer{};
                            auto draw_chain = [&](std::initializer_list<SkeletonPoint> order) {
                                int idx = 0;
                                for (auto point : order)
                                {
                                    const auto &node = get_bone(bones, point);
                                    if (!node.screen_valid)
                                        return;
                                    polyline_buffer[idx++] = node.screen;
                                }
                                if (idx >= 2)
                                {
                                    draw_list->AddPolyline(polyline_buffer.data(), idx, scale_color(state_color), false, skeleton_thickness);
                                    skeleton_drawn = true;
                                }
                            };

                            auto draw_segment = [&](SkeletonPoint a, SkeletonPoint b) {
                                const auto &na = get_bone(bones, a);
                                const auto &nb = get_bone(bones, b);
                                if (!na.screen_valid || !nb.screen_valid)
                                    return;
                                draw_list->AddLine(na.screen, nb.screen, scale_color(state_color), skeleton_thickness);
                                skeleton_drawn = true;
                            };

                            draw_chain({SkeletonPoint::Head, SkeletonPoint::Neck, SkeletonPoint::Collar, SkeletonPoint::Chest, SkeletonPoint::Pelvis});
                            draw_chain({SkeletonPoint::Collar, SkeletonPoint::LeftShoulder, SkeletonPoint::LeftElbow, SkeletonPoint::LeftHand});
                            draw_chain({SkeletonPoint::Collar, SkeletonPoint::RightShoulder, SkeletonPoint::RightElbow, SkeletonPoint::RightHand});
                            draw_chain({SkeletonPoint::Pelvis, SkeletonPoint::LeftHip, SkeletonPoint::LeftKnee, SkeletonPoint::LeftFoot});
                            draw_chain({SkeletonPoint::Pelvis, SkeletonPoint::RightHip, SkeletonPoint::RightKnee, SkeletonPoint::RightFoot});
                            draw_segment(SkeletonPoint::LeftShoulder, SkeletonPoint::RightShoulder);

                            if (skeleton_drawn)
                            {
                                rendered_this_player = true;
                                ++skeleton_draws_this_frame;
                            }
                        }
                    }
                }

                if (draw_head_circle && cached.head_valid)
                {
                    ImU32 circle_color = IM_COL32(255, 255, 255, 255);
                    if (head_circle_color_setting)
                    {
                        circle_color = IM_COL32(
                            static_cast<int>(head_circle_color_setting->value.color_val[0] * 255.0f),
                            static_cast<int>(head_circle_color_setting->value.color_val[1] * 255.0f),
                            static_cast<int>(head_circle_color_setting->value.color_val[2] * 255.0f),
                            static_cast<int>(head_circle_color_setting->value.color_val[3] * 255.0f));
                    }

                    float head_radius = std::clamp(raw_height * 0.18f, 4.0f, 28.0f);
                    draw_list->AddCircle(cached.head, head_radius, scale_color(circle_color), 32, 1.5f);
                    rendered_this_player = true;
                }

                if (draw_health)
                {
                    float ratio = std::clamp(current_health / current_max, 0.0f, 1.0f);
                    float bar_height = cached.max.y - cached.min.y;
                    float bar_width = 4.0f;
                    float bar_padding = 3.0f;
                    ImVec2 bar_min(cached.min.x - bar_width - bar_padding, cached.min.y);
                    ImVec2 bar_max(cached.min.x - bar_padding, cached.max.y);

                    draw_list->AddRectFilled(bar_min, bar_max, scale_color(IM_COL32(15, 15, 15, 180)));

                    float filled_height = bar_height * ratio;
                    ImVec2 filled_min(bar_min.x + 1.0f, bar_max.y - filled_height + 1.0f);
                    ImVec2 filled_max(bar_max.x - 1.0f, bar_max.y - 1.0f);
                    int red = static_cast<int>((1.0f - ratio) * 255.0f);
                    int green = static_cast<int>(ratio * 255.0f);
                    ImU32 health_color = IM_COL32(red, green, 0, 255);
                    draw_list->AddRectFilled(filled_min, filled_max, scale_color(health_color));

                    // optional divider ticks every 25%
                    constexpr int tick_count = 4;
                    for (int t = 1; t < tick_count; ++t)
                    {
                        float tick_y = bar_max.y - (bar_height * (t / static_cast<float>(tick_count)));
                        draw_list->AddLine(ImVec2(bar_min.x, tick_y), ImVec2(bar_max.x, tick_y), scale_color(IM_COL32(0, 0, 0, 120)), 1.0f);
                    }

                    rendered_this_player = true;
                }


                if (name_setting && name_setting->value.bool_val && !player.name.empty())
                {
                    ImVec2 text_size = ImGui::CalcTextSize(player.name.c_str());
                    float text_x = cached.min.x + ((cached.max.x - cached.min.x) - text_size.x) / 2.0f;
                    float text_y = cached.min.y - text_size.y - 2;

                    draw_list->AddText(
                        ImVec2(text_x + 1, text_y + 1),
                        scale_color(IM_COL32(0, 0, 0, 255)),
                        player.name.c_str());
                    draw_list->AddText(
                        ImVec2(text_x, text_y),
                        scale_color(IM_COL32(255, 255, 255, 255)),
                        player.name.c_str());

                    rendered_this_player = true;
                }

                if (need_distance_text)
                {
                    char dist_text[32];
                    snprintf(dist_text, sizeof(dist_text), "%.0fm", distance);

                    ImVec2 text_size = ImGui::CalcTextSize(dist_text);
                    float text_x = cached.min.x + ((cached.max.x - cached.min.x) - text_size.x) / 2.0f;
                    float text_y = cached.max.y + 2;

                    draw_list->AddText(
                        ImVec2(text_x + 1, text_y + 1),
                        scale_color(IM_COL32(0, 0, 0, 255)),
                        dist_text);
                    draw_list->AddText(
                        ImVec2(text_x, text_y),
                        scale_color(IM_COL32(255, 255, 255, 255)),
                        dist_text);

                    rendered_this_player = true;
                }

                if (rendered_this_player)
                    ++rendered_players;

                if (wallcheck_debug_enabled)
                {
                    engine::Wallcheck::VisibilityDebug debug;
                    if (engine::Wallcheck::get_debug_snapshot(player.character.address, debug))
                    {
                        char debug_text[192];
                        std::snprintf(debug_text, sizeof(debug_text),
                                      "rays %d/%d · cand %d · score %.1f/%.1f (head %.1f) · conf %.0f%% · dh %.1f · range %.0f · head %s · fallback %s · lat %s · vis %s",
                                      debug.rays_checked,
                                      debug.rays_enqueued,
                                      debug.candidate_parts,
                                      debug.visible_score,
                                      debug.required_score,
                                      debug.head_score,
                                      debug.confidence * 100.0f,
                                      debug.height_delta,
                                      debug.target_distance,
                                      debug.head_override ? "yes" : "no",
                                      debug.fallback_used ? "yes" : "no",
                                      debug.lateral_probe ? "yes" : "no",
                                      debug.final_result ? "yes" : "no");

                        ImVec2 text_pos(cached.min.x, cached.max.y + 14.0f);
                        draw_list->AddText(ImVec2(text_pos.x + 1.0f, text_pos.y + 1.0f), IM_COL32(0, 0, 0, 200), debug_text);
                        draw_list->AddText(text_pos, IM_COL32(255, 255, 0, 230), debug_text);
                    }
                }
            }

            draw_list->PopClipRect();

            double frame_ms = static_cast<double>(frame_profiler.refresh_time.count()) / 1'000'000.0;
            double attempts = static_cast<double>(frame_profiler.attempts);
            double successes = static_cast<double>(frame_profiler.successes);

            s_profile_refresh_ms += frame_ms;
            s_profile_attempts += attempts;
            s_profile_successes += successes;

            auto flush_elapsed = now - s_profile_last_flush;
            if (flush_elapsed >= kProfileFlushInterval)
            {
                double elapsed_ms = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(flush_elapsed).count());
                if (elapsed_ms <= 0.0)
                    elapsed_ms = 1.0;

                if (s_profile_attempts > 0.0)
                {
                    double avg_refresh = s_profile_refresh_ms / s_profile_attempts;
                    double attempts_per_sec = (s_profile_attempts * 1000.0) / elapsed_ms;
                    double success_per_sec = (s_profile_successes * 1000.0) / elapsed_ms;
                    cradle::profiling::record_esp_frame(avg_refresh, attempts_per_sec, success_per_sec);
                    s_profile_last_nonzero = now;
                }
                else if (s_profile_last_nonzero != std::chrono::steady_clock::time_point{} &&
                         now - s_profile_last_nonzero >= kProfileIdleInterval)
                {
                    cradle::profiling::record_esp_frame(0.0, 0.0, 0.0);
                    s_profile_last_nonzero = std::chrono::steady_clock::time_point{};
                }

                s_profile_refresh_ms = 0.0;
                s_profile_attempts = 0.0;
                s_profile_successes = 0.0;
                s_profile_last_flush = now;
            }

            if (rendered_players == 0)
            {
                ImVec2 debug_pos = viewport->Pos + ImVec2(32.0f, 32.0f);
                char debug_text[96];
                std::snprintf(debug_text, sizeof(debug_text), "ESP idle · snapshot %zu · eligible %zu · fallback %zu/%zu",
                              snapshot_players,
                              eligible_players,
                              fallback_success,
                              fallback_attempts);
                draw_list->AddText(debug_pos, IM_COL32(255, 80, 80, 220), debug_text);
            }

            for (auto it = g_cached_visuals.begin(); it != g_cached_visuals.end();)
            {
                if (now - it->second.last_seen > kForgetInterval)
                    it = g_cached_visuals.erase(it);
                else
                    ++it;
            }
    }
} // namespace cradle::modules
