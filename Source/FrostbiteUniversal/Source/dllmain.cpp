#include "FrostbiteRuntime.h"
#include "FrostbiteLog.h"

#include <Windows.h>

#include <cstring>
#include <exception>
#include <string>

namespace
{
    DWORD WINAPI BootstrapThreadBody(void* context)
    {
        const HMODULE selfModule = static_cast<HMODULE>(context);
        FrostbiteUniversal::Log::Initialize(selfModule);
        FrostbiteUniversal::Log::Write(L"Bootstrap thread started");

        const bool detected = FrostbiteUniversal::GetRuntime().Initialize();
        FrostbiteUniversal::Log::Write(detected
            ? L"Frostbite runtime detection: positive"
            : L"Frostbite runtime detection: negative");

        FrostbiteUniversal_PrimeGeneratedSdkCache(nullptr);
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

    DWORD BootstrapThreadWithCppGuards(void* context)
    {
        try
        {
            return BootstrapThreadBody(context);
        }
        catch (const std::exception& ex)
        {
            FrostbiteUniversal::Log::Write(L"Bootstrap thread caught a C++ exception");
            FrostbiteUniversal::Log::Write(std::wstring(L"Bootstrap exception: ") + std::wstring(ex.what(), ex.what() + std::strlen(ex.what())));
            return 0;
        }
        catch (...)
        {
            FrostbiteUniversal::Log::Write(L"Bootstrap thread caught an unknown C++ exception");
            return 0;
        }
    }

    DWORD WINAPI BootstrapThread(void* context)
    {
        DWORD exceptionCode = 0;
        __try
        {
            return BootstrapThreadWithCppGuards(context);
        }
        __except ((exceptionCode = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER)
        {
            wchar_t message[160] = {};
            swprintf_s(message, L"Bootstrap thread caught SEH exception 0x%08X", exceptionCode);
            FrostbiteUniversal::Log::Write(message);
            return 0;
        }
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
