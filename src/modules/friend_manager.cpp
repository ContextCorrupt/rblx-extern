#include "friend_manager.hpp"

#include <unordered_map>
#include <mutex>
#include <algorithm>
#include <cctype>

namespace cradle::friends
{
    namespace
    {
        std::unordered_map<std::string, std::string> g_friends;
        std::mutex g_mutex;

        std::string normalize(const std::string &name)
        {
            std::string trimmed = name;
            auto not_space_front = std::find_if(trimmed.begin(), trimmed.end(), [](unsigned char c) { return !std::isspace(c); });
            auto not_space_back = std::find_if(trimmed.rbegin(), trimmed.rend(), [](unsigned char c) { return !std::isspace(c); }).base();
            if (not_space_front < not_space_back)
                trimmed = std::string(not_space_front, not_space_back);
            else if (not_space_front == trimmed.end())
                trimmed.clear();

            std::string lower;
            lower.reserve(trimmed.size());
            for (unsigned char c : trimmed)
                lower.push_back(static_cast<char>(std::tolower(c)));
            return lower;
        }
    }

    void clear()
    {
        std::scoped_lock lock(g_mutex);
        g_friends.clear();
    }

    void add_friend(const std::string &name)
    {
        std::string key = normalize(name);
        if (key.empty())
            return;
        std::scoped_lock lock(g_mutex);
        g_friends[key] = name;
    }

    void remove_friend(const std::string &name)
    {
        std::string key = normalize(name);
        if (key.empty())
            return;
        std::scoped_lock lock(g_mutex);
        g_friends.erase(key);
    }

    bool toggle_friend(const std::string &name)
    {
        std::string key = normalize(name);
        if (key.empty())
            return false;

        std::scoped_lock lock(g_mutex);
        auto it = g_friends.find(key);
        if (it != g_friends.end())
        {
            g_friends.erase(it);
            return false;
        }
        g_friends[key] = name;
        return true;
    }

    bool is_friend(const std::string &name)
    {
        std::string key = normalize(name);
        if (key.empty())
            return false;
        std::scoped_lock lock(g_mutex);
        return g_friends.find(key) != g_friends.end();
    }

    std::vector<std::string> get_friends()
    {
        std::scoped_lock lock(g_mutex);
        std::vector<std::string> values;
        values.reserve(g_friends.size());
        for (const auto &entry : g_friends)
            values.push_back(entry.second);
        std::sort(values.begin(), values.end(), [](const std::string &a, const std::string &b) {
            std::string lower_a = a;
            std::string lower_b = b;
            std::transform(lower_a.begin(), lower_a.end(), lower_a.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::transform(lower_b.begin(), lower_b.end(), lower_b.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return lower_a < lower_b;
        });
        return values;
    }
}
