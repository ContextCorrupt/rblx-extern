#include "player_cache.hpp"
#include "../util/engine/offsets.hpp"
#include "../util/profiling/profiler.hpp"
#include <thread>

namespace cradle::engine
{

    namespace
    {
        constexpr auto kMinUpdateInterval = std::chrono::milliseconds(150);
    }

    std::shared_ptr<const std::vector<Player>> PlayerCache::players_snapshot = std::make_shared<std::vector<Player>>();
    std::unordered_map<std::uint64_t, Player> PlayerCache::entity_map;
    std::mutex PlayerCache::mtx;
    std::atomic<bool> PlayerCache::updating{false};
    std::chrono::steady_clock::time_point PlayerCache::last_tick = std::chrono::steady_clock::now();
    Player PlayerCache::local_player;
    bool PlayerCache::local_valid = false;
    static size_t last_children_count = 0;

    static Player build_player(Instance &char_inst, Instance & /*player_inst*/)
    {
        Player p;
        if (!char_inst.is_valid())
            return p;

        p.character = char_inst;
        p.humanoid = char_inst.find_first_child_of_class("Humanoid");
        p.head = char_inst.find_first_child("Head");

        if (p.humanoid.is_valid())
            p.rig_type = p.humanoid.get_rig_type();

        if (p.rig_type == 0)
        {
            p.hrp = char_inst.find_first_child("Torso");
        }
        else
        {
            p.hrp = char_inst.find_first_child("HumanoidRootPart");
            if (!p.hrp.is_valid())
                p.hrp = char_inst.find_first_child("Hitbox");
            if (!p.hrp.is_valid())
                p.hrp = char_inst.find_first_child("UpperTorso");
        }

        if (p.rig_type == 1)
        {
            p.upper_torso = char_inst.find_first_child("UpperTorso");
            p.lower_torso = char_inst.find_first_child("LowerTorso");
            p.right_upper_arm = char_inst.find_first_child("RightUpperArm");
            p.right_lower_arm = char_inst.find_first_child("RightLowerArm");
            p.right_hand = char_inst.find_first_child("RightHand");
            p.left_upper_arm = char_inst.find_first_child("LeftUpperArm");
            p.left_lower_arm = char_inst.find_first_child("LeftLowerArm");
            p.left_hand = char_inst.find_first_child("LeftHand");
            p.right_upper_leg = char_inst.find_first_child("RightUpperLeg");
            p.right_lower_leg = char_inst.find_first_child("RightLowerLeg");
            p.right_foot = char_inst.find_first_child("RightFoot");
            p.left_upper_leg = char_inst.find_first_child("LeftUpperLeg");
            p.left_lower_leg = char_inst.find_first_child("LeftLowerLeg");
            p.left_foot = char_inst.find_first_child("LeftFoot");
        }
        else
        {
            p.torso = p.upper_torso = char_inst.find_first_child("Torso");
            p.left_hand = char_inst.find_first_child("Left Arm");
            p.right_hand = char_inst.find_first_child("Right Arm");
            p.left_foot = char_inst.find_first_child("Left Leg");
            p.right_foot = char_inst.find_first_child("Right Leg");
        }

        if (p.humanoid.is_valid())
        {
            p.health = p.humanoid.get_health();
            p.max_health = p.humanoid.get_max_health();
        }
        else
        {
            p.health = p.max_health = 100.0f;
        }

        return p;
    }

    void PlayerCache::update_cache()
    {
        auto now = std::chrono::steady_clock::now();
        if (now - last_tick < kMinUpdateInterval)
            return;

        bool expected = false;
        if (!updating.compare_exchange_strong(expected, true))
            return;

        std::thread([]() {
            PlayerCache::rebuild_snapshot();
        }).detach();
    }

    void PlayerCache::rebuild_snapshot()
    {
        auto profile_start = std::chrono::high_resolution_clock::now();
        struct FlagReset
        {
            ~FlagReset()
            {
                PlayerCache::updating.store(false);
            }
        } flag_reset;

        DataModel dm = DataModel::get_instance();
        if (!dm.is_valid())
            return;

        Instance players_service = dm.get_players();
        if (!players_service.is_valid())
            return;

        auto children = players_service.get_children();
        auto now = std::chrono::steady_clock::now();
        bool recently = (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tick).count() < 100);
        if (recently && children.size() == last_children_count)
        {
            last_tick = now;
            return;
        }

        std::unordered_map<std::uint64_t, Player> working_map;
        {
            std::lock_guard<std::mutex> lock(mtx);
            working_map = entity_map;
        }

        if (children.size() > 50)
            working_map.clear();

        std::vector<Player> snapshot;
        snapshot.reserve(children.size());

        for (Instance &player : children)
        {
            if (!player.is_valid() || player.get_class_name() != "Player")
                continue;

            Instance character = player.get_character();
            std::string player_name = player.get_name();
            std::string player_team = player.get_team();
            auto player_addr = player.address;

            Player *entry_ptr = nullptr;
            auto cached = working_map.find(player_addr);

            if (cached != working_map.end())
            {
                bool same_character = character.is_valid() && cached->second.character.is_valid() &&
                                       cached->second.character.address == character.address;
                if (same_character && cached->second.is_valid())
                {
                    entry_ptr = &cached->second;
                }
            }

            if (!entry_ptr && character.is_valid())
            {
                Player entity = build_player(character, player);
                if (entity.is_valid())
                {
                    auto &stored = working_map[player_addr];
                    stored = entity;
                    entry_ptr = &stored;
                }
            }

            if (!entry_ptr)
            {
                if (cached != working_map.end())
                {
                    entry_ptr = &cached->second;
                }
                else
                {
                    Player placeholder;
                    placeholder.name = player_name;
                    placeholder.team = player_team;
                    auto &stored = working_map[player_addr];
                    stored = placeholder;
                    entry_ptr = &stored;
                }
            }

            if (!entry_ptr)
                continue;

            entry_ptr->name = player_name;
            entry_ptr->team = player_team;

            if (entry_ptr->humanoid.is_valid())
            {
                entry_ptr->health = entry_ptr->humanoid.get_health();
                entry_ptr->max_health = entry_ptr->humanoid.get_max_health();
            }
            else
            {
                entry_ptr->health = entry_ptr->max_health = 100.0f;
            }

            snapshot.push_back(*entry_ptr);
        }

        Player resolved_local;
        bool have_local = false;
        Instance local_instance = players_service.get_local_player();
        if (local_instance.is_valid())
        {
            auto player_it = working_map.find(local_instance.address);
            if (player_it != working_map.end())
            {
                resolved_local = player_it->second;
                have_local = resolved_local.is_valid();
            }
            else
            {
                Instance character = local_instance.get_character();
                if (character.is_valid())
                {
                    Player built = build_player(character, local_instance);
                    if (built.is_valid())
                    {
                        built.name = local_instance.get_name();
                        built.team = local_instance.get_team();
                        resolved_local = built;
                        have_local = true;
                        working_map[local_instance.address] = built;
                    }
                }
            }

            if (have_local)
            {
                resolved_local.name = local_instance.get_name();
                resolved_local.team = local_instance.get_team();
            }
        }

        auto snapshot_ptr = std::make_shared<std::vector<Player>>(std::move(snapshot));
        {
            std::lock_guard<std::mutex> lock(mtx);
            entity_map = std::move(working_map);
            players_snapshot = snapshot_ptr;
            local_player = have_local ? resolved_local : Player();
            local_valid = have_local;
            last_children_count = children.size();
            last_tick = std::chrono::steady_clock::now();
        }

        auto profile_end = std::chrono::high_resolution_clock::now();
        double update_ms = std::chrono::duration<double, std::milli>(profile_end - profile_start).count();
        cradle::profiling::record_playercache(update_ms, snapshot_ptr ? snapshot_ptr->size() : 0);
    }

    std::shared_ptr<const std::vector<Player>> PlayerCache::get_players()
    {
        std::lock_guard<std::mutex> lock(mtx);
        return players_snapshot;
    }

    Player PlayerCache::get_local_player()
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (local_valid)
            return local_player;
        return Player();
    }
}
