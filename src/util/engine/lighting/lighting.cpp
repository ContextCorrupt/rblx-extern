#include "lighting.hpp"
#include "../../memory/memory.hpp"
#include "../offsets.hpp"

namespace cradle::engine::lighting
{
    bool invalidate(const Instance &lighting)
    {
        if (!lighting.is_valid())
            return false;

        constexpr std::uint8_t kDirty = 0;
        return cradle::memory::write<std::uint8_t>(lighting.address + Offsets::Lighting::InvalidateFlag, kDirty);
    }
}
