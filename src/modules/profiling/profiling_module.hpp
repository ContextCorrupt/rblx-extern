#pragma once

#include "../module.hpp"

namespace cradle::modules
{
    class ProfilingModule : public Module
    {
    public:
        ProfilingModule();
        void on_render() override;
    };
}
