#include "instance.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <spdlog/spdlog.h>

namespace
{
    constexpr std::size_t kRobloxInlineStringThreshold = 16;
    constexpr std::size_t kRobloxMaxStringLength = 512;

    std::string read_ascii_string(uintptr_t address, std::size_t max_len)
    {
        if (!cradle::memory::IsValid(address))
            return {};

        std::string result;
        result.reserve(std::min<std::size_t>(max_len, 32));

        for (std::size_t i = 0; i < max_len; ++i)
        {
            char c = cradle::memory::read<char>(address + i);
            if (c == '\0')
                break;
            result.push_back(c);
        }

        return result;
    }

    std::string read_roblox_string(uintptr_t string_ptr)
    {
        if (!cradle::memory::IsValid(string_ptr))
            return {};

        auto safe_length = [&](uintptr_t offset) -> std::uint32_t {
            if (!cradle::memory::IsValid(string_ptr + offset))
                return 0;
            return cradle::memory::read<std::uint32_t>(string_ptr + offset);
        };

        std::uint32_t declared_length = safe_length(0x10);
        if (declared_length == 0 || declared_length > kRobloxMaxStringLength)
        {
            std::uint32_t alt = safe_length(0x18);
            if (alt > 0 && alt <= kRobloxMaxStringLength)
                declared_length = alt;
        }

        std::size_t max_read = declared_length > 0 ? std::min<std::size_t>(declared_length + 1, kRobloxMaxStringLength) : kRobloxMaxStringLength;
        bool prefer_inline = declared_length > 0 && declared_length < kRobloxInlineStringThreshold;

        if (prefer_inline)
        {
            auto inline_value = read_ascii_string(string_ptr, max_read);
            if (!inline_value.empty())
                return inline_value;
        }

        auto heap_ptr = cradle::memory::read<uintptr_t>(string_ptr);
        if (cradle::memory::IsValid(heap_ptr))
        {
            auto heap_value = read_ascii_string(heap_ptr, max_read);
            if (!heap_value.empty())
                return heap_value;
        }

        if (!prefer_inline)
        {
            auto fallback = read_ascii_string(string_ptr, kRobloxInlineStringThreshold);
            if (!fallback.empty())
                return fallback;
        }

        return {};
    }
}

namespace cradle::engine
{

    std::string Instance::get_name()
    {
        if (!address || address < 0x10000)
            return "null";

        std::uint64_t name_ptr = cradle::memory::read<std::uint64_t>(address + Offsets::Instance::Name);
        auto name = read_roblox_string(name_ptr);
        if (name.empty())
            return "unknown";
        return name;
    }

    std::string Instance::get_class_name() const
    {
        if (!address || address < 0x10000)
            return "null";

        std::uint64_t class_descriptor = cradle::memory::read<std::uint64_t>(address + Offsets::Instance::ClassDescriptor);
        if (!class_descriptor || class_descriptor < 0x10000)
            return "unknown";

        std::uint64_t class_name_ptr = cradle::memory::read<std::uint64_t>(class_descriptor + Offsets::Instance::ClassName);
        auto cls = read_roblox_string(class_name_ptr);
        if (cls.empty())
            return "unknown";
        return cls;
    }

    std::vector<Instance> Instance::get_children()
    {
        std::vector<Instance> children;
        if (!address || address < 0x10000)
            return children;

        std::uint64_t children_start = cradle::memory::read<std::uint64_t>(address + Offsets::Instance::ChildrenStart);
        if (!children_start || children_start < 0x10000)
            return children;

        std::uint64_t children_end = cradle::memory::read<std::uint64_t>(children_start + Offsets::Instance::ChildrenEnd);

        for (std::uint64_t i = cradle::memory::read<std::uint64_t>(children_start);
             i != children_end;
             i += 0x10)
        {
            std::uint64_t child_addr = cradle::memory::read<std::uint64_t>(i);
            if (child_addr && child_addr > 0x10000)
            {
                children.emplace_back(child_addr);
            }
        }

        return children;
    }

    Instance Instance::find_first_child(const std::string &name)
    {
        std::vector<Instance> children = get_children();
        for (auto &child : children)
        {
            if (child.get_name() == name)
            {
                return child;
            }
        }
        return Instance(0);
    }

    Instance Instance::find_first_child_of_class(const std::string &class_name)
    {
        std::vector<Instance> children = get_children();
        for (auto &child : children)
        {
            if (child.get_class_name() == class_name)
            {
                return child;
            }
        }
        return Instance(0);
    }

    std::vector<Instance> Instance::find_descendants_of_class(const std::string &class_name)
    {
        std::vector<Instance> result;
        std::vector<Instance> stack = get_children();

        while (!stack.empty())
        {
            Instance current = stack.back();
            stack.pop_back();

            if (current.get_class_name() == class_name)
            {
                result.push_back(current);
            }

            auto children = current.get_children();
            stack.insert(stack.end(), children.begin(), children.end());
        }

        return result;
    }

    Instance Instance::get_parent()
    {
        if (!address || address < 0x10000)
            return Instance(0);
        std::uint64_t parent_addr = cradle::memory::read<std::uint64_t>(address + Offsets::Instance::Parent);
        return Instance(parent_addr);
    }

    bool Instance::is_descendant_of(const Instance &ancestor) const
    {
        if (!ancestor.is_valid() || !address)
            return false;

        Instance current = *this;
        while (current.address != 0)
        {
            if (current.address == ancestor.address)
            {
                return true;
            }
            current = current.get_parent();
        }
        return false;
    }

    vector3 Instance::get_pos() const
    {
        if (!is_valid())
            return vector3(0, 0, 0);

        // camera instances store position directly at their camera offset
        std::string cls = get_class_name();
        if (cls == "Camera") {
            return cradle::memory::read<vector3>(address + Offsets::Camera::Position);
        }

        std::uint64_t prim = cradle::memory::read<std::uint64_t>(address + Offsets::BasePart::Primitive);
        if (!prim || prim <= 0x10000)
            return vector3(0, 0, 0);
        return cradle::memory::read<vector3>(prim + Offsets::BasePart::Position);
    }

    void Instance::set_pos(const vector3 &pos) const
    {
        if (!is_valid())
            return;

        std::uint64_t prim = cradle::memory::read<std::uint64_t>(address + Offsets::BasePart::Primitive);
        if (!prim || prim <= 0x10000)
            return;

        cradle::memory::write<vector3>(prim + Offsets::BasePart::Position, pos);
    }

    cframe Instance::get_cframe() const
    {
        if (!is_valid())
            return cframe();

        std::string cls = get_class_name();
        if (cls == "Camera") {
            matrix3 rot = cradle::memory::read<matrix3>(address + Offsets::Camera::Rotation);
            vector3 pos = cradle::memory::read<vector3>(address + Offsets::Camera::Position);
            return cframe(rot, pos);
        }

        std::uint64_t prim = cradle::memory::read<std::uint64_t>(address + Offsets::BasePart::Primitive);
        if (!prim || prim <= 0x10000)
            return cframe();
        return cradle::memory::read<cframe>(prim + Offsets::BasePart::Rotation);
    }

    Instance Instance::get_character() const
    {
        if (!is_valid())
            return Instance(0);
        std::uint64_t char_addr = cradle::memory::read<std::uint64_t>(address + Offsets::Player::ModelInstance);
        return Instance(char_addr);
    }

    Instance Instance::get_local_player() const
    {
        if (!is_valid())
            return Instance(0);
        std::uint64_t local_addr = cradle::memory::read<std::uint64_t>(address + Offsets::Player::LocalPlayer);
        return Instance(local_addr);
    }

    float Instance::get_health() const
    {
        if (!is_valid())
            return 0.0f;

        float health = cradle::memory::read<float>(address + Offsets::Humanoid::Health);
        if (!std::isfinite(health))
        {
            health = 0.0f;
        }
        return std::clamp(health, 0.0f, 1000.0f);
    }

    float Instance::get_max_health() const
    {
        if (!is_valid())
            return 100.0f;

        float max_health = cradle::memory::read<float>(address + Offsets::Humanoid::MaxHealth);
        if (!std::isfinite(max_health) || max_health <= 0.0f)
        {
            max_health = 100.0f;
        }
        return std::clamp(max_health, 1.0f, 5000.0f);
    }

    float Instance::get_transparency() const
    {
        if (!is_valid())
            return 1.0f;

        std::uint64_t prim = cradle::memory::read<std::uint64_t>(address + Offsets::BasePart::Primitive);
        if (!prim || prim <= 0x10000)
            return 1.0f;

        float transparency = cradle::memory::read<float>(prim + Offsets::BasePart::Transparency);
        if (!std::isfinite(transparency))
            return 1.0f;

        return std::clamp(transparency, 0.0f, 1.0f);
    }

    std::uint8_t Instance::get_visible_flag_raw() const
    {
        if (!is_valid())
            return 0;
        return cradle::memory::read<std::uint8_t>(address + Offsets::GuiObject::Visible);
    }

    bool Instance::get_visible_flag() const
    {
        std::uint8_t flag = get_visible_flag_raw();
        if (flag == 1)
            return false;
        return true;
    }

    int Instance::get_rig_type() const
    {
        if (!is_valid())
            return 0;
        return cradle::memory::read<int>(address + Offsets::Humanoid::RigType);
    }

    std::string Instance::get_team() const
    {
        if (!address || address < 0x10000)
            return "Unknown";

        std::uint64_t team_ptr = cradle::memory::read<std::uint64_t>(address + Offsets::Player::Team);
        if (!team_ptr || team_ptr < 0x10000)
            return "Unknown";

        std::uint64_t name_ptr = cradle::memory::read<std::uint64_t>(team_ptr + Offsets::Instance::Name);
        if (!name_ptr || name_ptr < 0x10000)
            return "Unknown";

        auto team_name = read_roblox_string(name_ptr);
        if (team_name.empty())
            return "Unknown";
        return team_name;
    }
}
