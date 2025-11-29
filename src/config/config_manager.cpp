#include "config_manager.hpp"
#include "../modules/module_manager.hpp"
#include "../modules/module.hpp"
#include "../modules/friend_manager.hpp"
#include "../modules/anti_aim/anti_aim_module.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>

namespace cradle::config
{
    namespace
    {
        constexpr const char *kConfigPath = "config/mossad_config.cfg";
        constexpr std::chrono::milliseconds kAutoSaveDelay(1500);

        bool g_initialized = false;
        bool g_dirty = false;
        bool g_loading = false;
        std::chrono::steady_clock::time_point g_last_save = std::chrono::steady_clock::now();

        void disable_all_features(modules::ModuleManager &manager)
        {
            bool previous_loading = g_loading;
            g_loading = true;
            auto modules = manager.get_all_modules();
            for (auto *module : modules)
            {
                module->set_enabled(false);
                for (auto &setting : module->get_settings())
                {
                    if (setting.type == modules::SettingType::BOOL)
                    {
                        setting.value.bool_val = false;
                    }
                }
            }
            g_loading = previous_loading;
            friends::clear();
        }

        std::string trim(const std::string &str)
        {
            const char *whitespace = " \t\r\n";
            size_t start = str.find_first_not_of(whitespace);
            if (start == std::string::npos)
                return "";
            size_t end = str.find_last_not_of(whitespace);
            return str.substr(start, end - start + 1);
        }

        modules::Module *find_module(modules::ModuleManager &manager, const std::string &name)
        {
            for (auto *mod : manager.get_all_modules())
            {
                if (_stricmp(mod->get_name().c_str(), name.c_str()) == 0)
                {
                    return mod;
                }
            }
            return nullptr;
        }

        void ensure_directory()
        {
            std::filesystem::create_directories(std::filesystem::path(kConfigPath).parent_path());
        }

        bool parse_bool(const std::string &value)
        {
            return value == "1" || value == "true" || value == "True";
        }

        void apply_setting(modules::Module *module, modules::Setting *setting, const std::string &value)
        {
            if (!setting)
                return;

            (void)module;

            try
            {
                switch (setting->type)
                {
                case modules::SettingType::BOOL:
                    setting->value.bool_val = parse_bool(value);
                    break;
                case modules::SettingType::INT:
                    setting->value.int_val = std::clamp(std::stoi(value), setting->range.int_range.min, setting->range.int_range.max);
                    break;
                case modules::SettingType::FLOAT:
                    {
                        float f = std::stof(value);
                        f = std::clamp(f, setting->range.float_range.min, setting->range.float_range.max);
                        setting->value.float_val = f;
                        break;
                    }
                case modules::SettingType::COLOR:
                    {
                        float comps[4] = {setting->value.color_val[0], setting->value.color_val[1], setting->value.color_val[2], setting->value.color_val[3]};
                        std::stringstream ss(value);
                        std::string token;
                        int idx = 0;
                        while (std::getline(ss, token, ',') && idx < 4)
                        {
                            comps[idx++] = std::stof(trim(token));
                        }
                        for (int i = 0; i < 4; ++i)
                        {
                            comps[i] = std::clamp(comps[i], 0.0f, 1.0f);
                            setting->value.color_val[i] = comps[i];
                        }
                        break;
                    }
                }
            }
            catch (...)
            {
                // ignore malformed values
            }
        }
    }

    void ConfigManager::initialize(modules::ModuleManager &manager)
    {
        if (g_initialized)
            return;

        bool loaded = load(manager);
        if (!loaded)
        {
            disable_all_features(manager);
            ensure_directory();
            save(manager);
        }
        manager.update_keybinds();
        g_initialized = true;
        g_last_save = std::chrono::steady_clock::now();
        g_dirty = false;
    }

    void ConfigManager::tick(modules::ModuleManager &manager, bool force)
    {
        if (!g_initialized)
            return;

        auto now = std::chrono::steady_clock::now();
        if (g_dirty && (force || now - g_last_save >= kAutoSaveDelay))
        {
            save(manager);
            g_last_save = now;
            g_dirty = false;
        }
    }

    void ConfigManager::mark_dirty()
    {
        if (!g_loading && g_initialized)
        {
            g_dirty = true;
        }
    }

    void ConfigManager::save_now(modules::ModuleManager &manager)
    {
        save(manager);
        g_last_save = std::chrono::steady_clock::now();
        g_dirty = false;
    }

    void ConfigManager::load_now(modules::ModuleManager &manager)
    {
        bool loaded = load(manager);
        if (!loaded)
        {
            disable_all_features(manager);
            ensure_directory();
            save(manager);
        }
        manager.update_keybinds();
        g_dirty = false;
    }

    void ConfigManager::reset_to_defaults(modules::ModuleManager &manager)
    {
        disable_all_features(manager);
        manager.update_keybinds();
        mark_dirty();
    }

    bool ConfigManager::load(modules::ModuleManager &manager)
    {
        g_loading = true;
        friends::clear();
        std::ifstream input(kConfigPath);
        if (!input.is_open())
        {
            g_loading = false;
            return false;
        }

        std::string line;
        while (std::getline(input, line))
        {
            line = trim(line);
            if (line.empty() || line[0] == '#')
                continue;

            auto sep = line.find('=');
            if (sep == std::string::npos)
                continue;

            std::string key = trim(line.substr(0, sep));
            std::string value = trim(line.substr(sep + 1));

            auto colon = key.find(':');
            std::string module_name = colon == std::string::npos ? key : key.substr(0, colon);
            std::string setting_name = colon == std::string::npos ? std::string() : key.substr(colon + 1);

            if (_stricmp(module_name.c_str(), "friend") == 0 || _stricmp(module_name.c_str(), "friends") == 0)
            {
                if (!setting_name.empty() && parse_bool(value))
                {
                    friends::add_friend(setting_name);
                }
                continue;
            }

            auto *module = find_module(manager, module_name);
            if (!module)
                continue;

            if (setting_name.empty())
            {
                module->set_enabled(parse_bool(value));
                continue;
            }

            if (setting_name == "keybind")
            {
                try { module->set_keybind(std::stoi(value)); } catch (...) {}
                continue;
            }

            if (setting_name == "keybind_mode")
            {
                try {
                    int mode = std::stoi(value);
                    module->set_keybind_mode(mode == static_cast<int>(modules::KeybindMode::HOLD) ? modules::KeybindMode::HOLD : modules::KeybindMode::TOGGLE);
                } catch (...) {}
                continue;
            }

            if (auto *anti_module = dynamic_cast<modules::AntiAimModule *>(module))
            {
                if (setting_name == "desync_hotkey")
                {
                    try { anti_module->set_desync_hotkey(std::stoi(value)); } catch (...) {}
                    continue;
                }

                if (setting_name == "desync_hotkey_mode")
                {
                    try
                    {
                        int mode = std::stoi(value);
                        anti_module->set_desync_hotkey_mode(mode == static_cast<int>(modules::KeybindMode::HOLD) ? modules::KeybindMode::HOLD : modules::KeybindMode::TOGGLE);
                    }
                    catch (...)
                    {
                    }
                    continue;
                }
            }

            auto *setting = module->get_setting(setting_name);
            if (!setting)
                continue;

            apply_setting(module, setting, value);
        }

        g_loading = false;
        return true;
    }

    void ConfigManager::save(modules::ModuleManager &manager)
    {
        ensure_directory();
        std::ofstream output(kConfigPath, std::ios::trunc);
        if (!output.is_open())
            return;

        output << "# Mossad.is auto configuration\n";
        auto modules = manager.get_all_modules();
        for (auto *module : modules)
        {
            output << module->get_name() << '=' << (module->is_enabled() ? 1 : 0) << '\n';
            output << module->get_name() << ":keybind=" << module->get_keybind() << '\n';
            output << module->get_name() << ":keybind_mode=" << static_cast<int>(module->get_keybind_mode()) << '\n';

            for (auto &setting : module->get_settings())
            {
                output << module->get_name() << ':' << setting.name << '=';
                switch (setting.type)
                {
                case modules::SettingType::BOOL:
                    output << (setting.value.bool_val ? 1 : 0);
                    break;
                case modules::SettingType::INT:
                    output << setting.value.int_val;
                    break;
                case modules::SettingType::FLOAT:
                    output << std::fixed << std::setprecision(4) << setting.value.float_val;
                    break;
                case modules::SettingType::COLOR:
                    output << std::fixed << std::setprecision(3)
                           << setting.value.color_val[0] << ','
                           << setting.value.color_val[1] << ','
                           << setting.value.color_val[2] << ','
                           << setting.value.color_val[3];
                    break;
                }
                output << '\n';
            }

            if (auto *anti_module = dynamic_cast<modules::AntiAimModule *>(module))
            {
                output << module->get_name() << ":desync_hotkey=" << anti_module->get_desync_hotkey() << '\n';
                output << module->get_name() << ":desync_hotkey_mode=" << static_cast<int>(anti_module->get_desync_hotkey_mode()) << '\n';
            }

            output << '\n';
        }

        auto friends_list = friends::get_friends();
        if (!friends_list.empty())
        {
            for (const auto &name : friends_list)
            {
                output << "friend:" << name << "=1\n";
            }
            output << '\n';
        }
    }

    void mark_dirty()
    {
        ConfigManager::mark_dirty();
    }
}
