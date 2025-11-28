#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <atomic>
#include <string>
#include <mutex>
#include "../modules/module.hpp"

namespace cradle
{
    namespace overlay
    {
        class Overlay
        {
        public:
            enum class LoadingStage
            {
                WaitingForRoblox = 0,
                Attaching,
                Initializing,
                Ready,
                Failed
            };

        private:
            ID3D11Device *device = nullptr;
            ID3D11DeviceContext *context = nullptr;
            IDXGISwapChain *swapchain = nullptr;
            ID3D11RenderTargetView *renderTargetView = nullptr;
            HWND overlayWindow = nullptr;
            bool showMenu = false;
            modules::Module* selected_module = nullptr;
            int selected_tab = 0;
            
            static UINT resize_width;
            static UINT resize_height;
            static Overlay* instance;
            static bool menu_visible;
            static std::atomic<bool> runtime_ready;

            static std::atomic<LoadingStage> loading_stage;
            static std::mutex loading_message_mutex;
            static std::string loading_message;

        public:
            bool initialize();
            void cleanup();
            void render();
            bool isRunning();
            static bool is_menu_open() { return menu_visible; }
            static HWND get_overlay_window();
            static POINT get_overlay_origin();

            static void set_loading_stage(LoadingStage stage, const std::string &message);
            static LoadingStage get_loading_stage();
            static std::string get_loading_message();
            static void set_runtime_ready(bool ready);
            static bool is_runtime_ready();

            static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

        private:
            void createWindow();
            bool setupDirectX();
            bool setupImGui();
            void createRenderTarget();
            void cleanupRenderTarget();
            void updateWindowPosition();
            void updateInputPassthrough();
        };

        
    }
}
