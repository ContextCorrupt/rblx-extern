#include "hwid.hpp"

#include <Wbemidl.h>
#include <Winhttp.h>
#include <comdef.h>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "winhttp.lib")

namespace cradle::security
{
    namespace
    {
        constexpr wchar_t kWhitelistHost[] = L"raw.githubusercontent.com";
        constexpr wchar_t kWhitelistPath[] = L"/07zj/pciehwid/refs/heads/main/list";

        std::filesystem::path ResolveExecutableDirectory()
        {
            wchar_t buffer[MAX_PATH];
            DWORD copied = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
            if (copied == 0)
                return std::filesystem::current_path();
            std::filesystem::path exe_path(buffer, buffer + copied);
            return exe_path.parent_path();
        }

        std::string TrimCopy(const std::string &text)
        {
            auto it_begin = std::find_if_not(text.begin(), text.end(), [](unsigned char ch) {
                return std::isspace(ch) != 0;
            });
            auto it_end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char ch) {
                return std::isspace(ch) != 0;
            }).base();

            if (it_begin >= it_end)
                return {};
            return std::string(it_begin, it_end);
        }

        bool EqualsIgnoreCase(const std::string &lhs, const std::string &rhs)
        {
            if (lhs.size() != rhs.size())
                return false;

            for (std::size_t i = 0; i < lhs.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(lhs[i])) != std::tolower(static_cast<unsigned char>(rhs[i])))
                    return false;
            }
            return true;
        }

        void AppendWhitelistLine(const std::string &line, std::vector<std::string> &target)
        {
            std::string trimmed = TrimCopy(line);
            if (trimmed.empty())
                return;
            if (trimmed[0] == '#')
                return;
            target.push_back(trimmed);
        }

        std::vector<std::string> ParseWhitelistText(const std::string &text)
        {
            std::vector<std::string> entries;
            std::istringstream stream(text);
            std::string line;
            while (std::getline(stream, line))
                AppendWhitelistLine(line, entries);
            return entries;
        }

        bool HttpGet(const wchar_t *host, const wchar_t *path, std::string &response_body)
        {
            HINTERNET session = WinHttpOpen(L"CradleLoader/1.0",
                                            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                            WINHTTP_NO_PROXY_NAME,
                                            WINHTTP_NO_PROXY_BYPASS,
                                            0);
            if (!session)
                return false;

            HINTERNET connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
            if (!connection)
            {
                WinHttpCloseHandle(session);
                return false;
            }

            HINTERNET request = WinHttpOpenRequest(connection,
                                                   L"GET",
                                                   path,
                                                   nullptr,
                                                   WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                   WINHTTP_FLAG_SECURE | WINHTTP_FLAG_REFRESH);
            if (!request)
            {
                WinHttpCloseHandle(connection);
                WinHttpCloseHandle(session);
                return false;
            }

            BOOL ok = WinHttpSendRequest(request,
                                         WINHTTP_NO_ADDITIONAL_HEADERS,
                                         0,
                                         WINHTTP_NO_REQUEST_DATA,
                                         0,
                                         0,
                                         0);
            if (!ok)
            {
                WinHttpCloseHandle(request);
                WinHttpCloseHandle(connection);
                WinHttpCloseHandle(session);
                return false;
            }

            ok = WinHttpReceiveResponse(request, nullptr);
            if (!ok)
            {
                WinHttpCloseHandle(request);
                WinHttpCloseHandle(connection);
                WinHttpCloseHandle(session);
                return false;
            }

            std::string body;
            DWORD available = 0;
            do
            {
                if (!WinHttpQueryDataAvailable(request, &available))
                {
                    WinHttpCloseHandle(request);
                    WinHttpCloseHandle(connection);
                    WinHttpCloseHandle(session);
                    return false;
                }

                if (available == 0)
                    break;

                std::string chunk;
                chunk.resize(available);
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), available, &read))
                {
                    WinHttpCloseHandle(request);
                    WinHttpCloseHandle(connection);
                    WinHttpCloseHandle(session);
                    return false;
                }

                chunk.resize(read);
                body += chunk;
            } while (available > 0);

            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);

            response_body = std::move(body);
            return true;
        }

        std::vector<std::string> LoadRemoteWhitelist()
        {
            std::string body;
            if (!HttpGet(kWhitelistHost, kWhitelistPath, body))
                return {};
            return ParseWhitelistText(body);
        }

        std::vector<std::string> LoadLocalWhitelistFile()
        {
            std::vector<std::string> entries;
            auto path = GetWhitelistFilePath();
            std::ifstream file(path);
            if (!file.is_open())
                return entries;

            std::string line;
            while (std::getline(file, line))
                AppendWhitelistLine(line, entries);

            return entries;
        }

        struct ComScope
        {
            explicit ComScope()
            {
                HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                initialized = SUCCEEDED(hr);
                uninitialize = hr == S_OK || hr == S_FALSE;
                init_result = hr;
            }

            ~ComScope()
            {
                if (uninitialize)
                    CoUninitialize();
            }

            bool ok() const
            {
                return init_result == S_OK || init_result == S_FALSE || init_result == RPC_E_CHANGED_MODE;
            }

            bool initialized = false;
            bool uninitialize = false;
            HRESULT init_result = E_FAIL;
        };

        bool InitializeSecurity()
        {
            HRESULT hr = CoInitializeSecurity(nullptr,
                                               -1,
                                               nullptr,
                                               nullptr,
                                               RPC_C_AUTHN_LEVEL_DEFAULT,
                                               RPC_C_IMP_LEVEL_IMPERSONATE,
                                               nullptr,
                                               EOAC_NONE,
                                               nullptr);
            return SUCCEEDED(hr) || hr == RPC_E_TOO_LATE;
        }

        std::vector<std::string> QueryGpuDeviceIds()
        {
            std::vector<std::string> device_ids;

            ComScope com_scope;
            if (!com_scope.ok())
                return device_ids;

            if (!InitializeSecurity())
                return device_ids;

            IWbemLocator *locator = nullptr;
            HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_IWbemLocator, reinterpret_cast<LPVOID *>(&locator));
            if (FAILED(hr) || !locator)
                return device_ids;

            IWbemServices *services = nullptr;
            hr = locator->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
            if (FAILED(hr) || !services)
            {
                locator->Release();
                return device_ids;
            }

            hr = CoSetProxyBlanket(services,
                                   RPC_C_AUTHN_WINNT,
                                   RPC_C_AUTHZ_NONE,
                                   nullptr,
                                   RPC_C_AUTHN_LEVEL_CALL,
                                   RPC_C_IMP_LEVEL_IMPERSONATE,
                                   nullptr,
                                   EOAC_NONE);
            if (FAILED(hr))
            {
                services->Release();
                locator->Release();
                return device_ids;
            }

            IEnumWbemClassObject *enumerator = nullptr;
            hr = services->ExecQuery(bstr_t("WQL"),
                                     bstr_t("SELECT PNPDeviceID FROM Win32_VideoController"),
                                     WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                     nullptr,
                                     &enumerator);
            if (FAILED(hr) || !enumerator)
            {
                services->Release();
                locator->Release();
                return device_ids;
            }

            while (true)
            {
                IWbemClassObject *object = nullptr;
                ULONG returned = 0;
                HRESULT next_hr = enumerator->Next(WBEM_INFINITE, 1, &object, &returned);
                if (FAILED(next_hr) || returned == 0)
                    break;

                VARIANT value;
                VariantInit(&value);
                HRESULT get_hr = object->Get(L"PNPDeviceID", 0, &value, nullptr, nullptr);
                if (SUCCEEDED(get_hr) && value.vt == VT_BSTR && value.bstrVal != nullptr)
                {
                    _bstr_t bstr_value(value.bstrVal);
                    std::string id = static_cast<const char *>(bstr_value);
                    if (!id.empty())
                        device_ids.push_back(id);
                }

                VariantClear(&value);
                object->Release();
            }

            enumerator->Release();
            services->Release();
            locator->Release();

            return device_ids;
        }
    } // namespace

    std::filesystem::path GetWhitelistFilePath()
    {
        static std::filesystem::path cached = ResolveExecutableDirectory() / "config" / "hwid_whitelist.txt";
        return cached;
    }

    std::vector<std::string> LoadGpuWhitelist()
    {
        std::vector<std::string> whitelist = LoadRemoteWhitelist();
        auto local_entries = LoadLocalWhitelistFile();
        whitelist.insert(whitelist.end(), local_entries.begin(), local_entries.end());
        return whitelist;
    }

    HardwareValidationResult ValidateGpuWhitelist(const std::vector<std::string> &allowed_ids)
    {
        HardwareValidationResult result;
        auto detected_ids = QueryGpuDeviceIds();
        if (detected_ids.empty())
        {
            result.message = "Unable to query GPU PCI IDs via WMI";
            return result;
        }

        result.detected_id = detected_ids.front();

        if (allowed_ids.empty())
        {
            result.message = "No GPU HWIDs are listed in config/hwid_whitelist.txt";
            return result;
        }

        for (const auto &detected : detected_ids)
        {
            for (const auto &allowed : allowed_ids)
            {
                if (EqualsIgnoreCase(detected, allowed))
                {
                    result.authorized = true;
                    result.detected_id = detected;
                    return result;
                }
            }
        }

        result.message = "Detected GPU HWID is not authorized";
        return result;
    }
}
