#include "esp_visuals_module.hpp"

namespace cradle::modules
{
    ESPVisualsModule::ESPVisualsModule()
        : Module("visual colors", "adjust esp color accents")
    {
        enabled = true;
    }
}
