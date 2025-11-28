#pragma once
#include "player.hpp"
#include "../util/engine/datamodel/datamodel.hpp"
#include <vector>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <chrono>
#include <memory>

namespace cradle::engine
{

    class PlayerCache
    {
    private:
        static std::shared_ptr<const std::vector<Player>> players_snapshot;
        static std::unordered_map<std::uint64_t, Player> entity_map;
        static std::mutex mtx;
        static std::atomic<bool> updating;
        static std::chrono::steady_clock::time_point last_tick;
        static Player local_player;
        static bool local_valid;
        static void rebuild_snapshot();
    public:
        static void update_cache();

        static std::shared_ptr<const std::vector<Player>> get_players();
        static Player get_local_player();
        static bool is_updating() { return updating.load(); }
    };
}
