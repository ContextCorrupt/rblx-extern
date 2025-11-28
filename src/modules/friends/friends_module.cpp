#include "friends_module.hpp"

namespace cradle::modules
{
    FriendsModule::FriendsModule()
        : Module("friends", "manage friendly players")
    {
        enabled = true;
    }
}
