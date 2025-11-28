#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace cradle::security
{
    struct HardwareValidationResult
    {
        bool authorized = false;
        std::string detected_id;
        std::string message;
    };

    // Loads GPU HWIDs from config/hwid_whitelist.txt (one per line, '#' comments allowed).
    std::vector<std::string> LoadGpuWhitelist();

    // Returns true when at least one detected GPU ID matches the whitelist.
    HardwareValidationResult ValidateGpuWhitelist(const std::vector<std::string> &allowed_ids);

    // Exposes the resolved whitelist file path so callers can show it in errors/help text.
    std::filesystem::path GetWhitelistFilePath();
}
