#include "FrostbiteRuntime.h"
#include "FrostbiteLog.h"

#include <Windows.h>

namespace
{
    DWORD WINAPI BootstrapThread(void* context)
    {
        const HMODULE selfModule = static_cast<HMODULE>(context);
        FrostbiteUniversal::Log::Initialize(selfModule);
        FrostbiteUniversal::Log::Write(L"Bootstrap thread started");

        const bool detected = FrostbiteUniversal::GetRuntime().Initialize();
        FrostbiteUniversal::Log::Write(detected
            ? L"Frostbite runtime detection: positive"
            : L"Frostbite runtime detection: negative");

        FrostbiteUniversal::Log::Write(L"Project bridge is lazy: actor/model and timescale host calls wait for explicit ImGui/API requests");

        if (FrostbiteUniversal_HasSharedImGui())
        {
            const int hotkeyStarted = FrostbiteUniversal_ImGuiStartHotkeyMonitor();
            FrostbiteUniversal::Log::Write(hotkeyStarted
                ? L"ImGui F4 hotkey monitor bootstrap: started"
                : L"ImGui F4 hotkey monitor bootstrap: failed");

            const int overlayStarted = FrostbiteUniversal_OverlayStart();
            FrostbiteUniversal::Log::Write(overlayStarted
                ? L"Self-hosted overlay bootstrap: started"
                : L"Self-hosted overlay bootstrap: failed");
        }
        return 0;
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        ::DisableThreadLibraryCalls(module);

        if (HANDLE thread = ::CreateThread(nullptr, 0, BootstrapThread, module, 0, nullptr))
            ::CloseHandle(thread);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        FrostbiteUniversal::Log::Write(L"DLL_PROCESS_DETACH received");
        FrostbiteUniversal_OverlayStop();
        FrostbiteUniversal_ImGuiShutdown();
        FrostbiteUniversal::GetRuntime().Shutdown();
        FrostbiteUniversal::Log::Shutdown();
    }

    return TRUE;
}
