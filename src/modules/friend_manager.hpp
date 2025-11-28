#pragma once

#include <string>
#include <vector>

namespace cradle::friends
{
    void clear();
    void add_friend(const std::string &name);
    void remove_friend(const std::string &name);
    bool toggle_friend(const std::string &name);
    bool is_friend(const std::string &name);
    std::vector<std::string> get_friends();
}
