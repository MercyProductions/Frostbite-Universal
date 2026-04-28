#include "FrostbiteLog.h"
#include "FrostbiteUniversal.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace fs = std::filesystem;

namespace
{
    std::mutex g_logMutex;
    bool g_initialized = false;
    bool g_consoleOpen = false;
    fs::path g_logPath;

    std::wstring GetEnvVar(const wchar_t* name)
    {
        wchar_t buffer[32767] = {};
        const DWORD length = ::GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
        if (length == 0 || length >= std::size(buffer))
            return {};

        return buffer;
    }

    std::wstring GetModulePath(HMODULE module)
    {
        wchar_t path[MAX_PATH] = {};
        ::GetModuleFileNameW(module, path, MAX_PATH);
        return path;
    }

    std::wstring GetProcessPath()
    {
        wchar_t path[MAX_PATH] = {};
        ::GetModuleFileNameW(nullptr, path, MAX_PATH);
        return path;
    }

    std::wstring FileNameFromPath(const std::wstring& path)
    {
        const std::size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? path : path.substr(slash + 1);
    }

    fs::path BuildLogPath()
    {
        const std::wstring userProfile = GetEnvVar(L"USERPROFILE");
        fs::path base = userProfile.empty()
            ? fs::temp_directory_path() / L"Frostbite"
            : fs::path(userProfile) / L"Desktop" / L"Frostbite" / L"Logs";

        std::error_code ec;
        fs::create_directories(base, ec);
        if (ec)
            base = fs::temp_directory_path();

        std::wstringstream name;
        name << L"FrostbiteUniversal_" << ::GetCurrentProcessId() << L".log";
        return base / name.str();
    }

    std::wstring Timestamp()
    {
        SYSTEMTIME time = {};
        ::GetLocalTime(&time);

        wchar_t buffer[64] = {};
        swprintf_s(
            buffer,
            L"%04hu-%02hu-%02hu %02hu:%02hu:%02hu.%03hu",
            time.wYear,
            time.wMonth,
            time.wDay,
            time.wHour,
            time.wMinute,
            time.wSecond,
            time.wMilliseconds);
        return buffer;
    }

    std::wstring LastErrorText(DWORD error)
    {
        if (error == ERROR_SUCCESS)
            return L"0";

        wchar_t* message = nullptr;
        const DWORD length = ::FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            error,
            0,
            reinterpret_cast<wchar_t*>(&message),
            0,
            nullptr);

        std::wstring result = std::to_wstring(error);
        if (length != 0 && message)
        {
            result += L" (";
            result.append(message, length);
            while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' '))
                result.pop_back();
            result += L")";
        }

        if (message)
            ::LocalFree(message);

        return result;
    }

    void WriteUnlocked(std::wstring_view message)
    {
        if (g_logPath.empty())
            g_logPath = BuildLogPath();

        std::wofstream file(g_logPath, std::ios::app);
        if (file)
            file << L"[" << Timestamp() << L"] " << message << L"\n";

        std::wstring debugLine(message);
        debugLine += L"\n";
        ::OutputDebugStringW(debugLine.c_str());

        if (g_consoleOpen)
        {
            DWORD written = 0;
            const std::wstring consoleLine = L"[FrostbiteUniversal] " + std::wstring(message) + L"\n";
            ::WriteConsoleW(::GetStdHandle(STD_OUTPUT_HANDLE), consoleLine.c_str(), static_cast<DWORD>(consoleLine.size()), &written, nullptr);
        }
    }

    bool OpenConsoleUnlocked()
    {
        if (g_consoleOpen)
            return true;

        const bool hasConsoleWindow = ::GetConsoleWindow() != nullptr;
        bool openedConsole = hasConsoleWindow;

        if (!openedConsole)
        {
            if (::AttachConsole(ATTACH_PARENT_PROCESS))
            {
                openedConsole = true;
            }
            else
            {
                const DWORD attachError = ::GetLastError();
                if (::AllocConsole())
                {
                    openedConsole = true;
                }
                else
                {
                    const DWORD allocError = ::GetLastError();
                    WriteUnlocked(
                        L"Console open failed: AttachConsole=" + LastErrorText(attachError) +
                        L", AllocConsole=" + LastErrorText(allocError));
                    return false;
                }
            }
        }

        if (!openedConsole)
        {
            WriteUnlocked(L"Console open failed: no console window or handles available");
            return false;
        }

        FILE* stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
        freopen_s(&stream, "CONIN$", "r", stdin);

        ::SetConsoleTitleW(L"FrostbiteUniversal");
        g_consoleOpen = true;
        WriteUnlocked(L"Console opened");
        return true;
    }
}

namespace FrostbiteUniversal::Log
{
    void Initialize(HMODULE selfModule)
    {
        std::lock_guard lock(g_logMutex);
        if (g_initialized)
        {
#if FROSTBITEUNIVERSAL_AUTO_CONSOLE
            OpenConsoleUnlocked();
#endif
            return;
        }

        g_logPath = BuildLogPath();
        g_initialized = true;

        WriteUnlocked(L"============================================================");
        WriteUnlocked(L"FrostbiteUniversal loaded");
        WriteUnlocked(L"Process: " + FileNameFromPath(GetProcessPath()));
        WriteUnlocked(L"Process path: " + GetProcessPath());
        WriteUnlocked(L"DLL path: " + GetModulePath(selfModule));
        WriteUnlocked(L"Log path: " + g_logPath.wstring());

#if FROSTBITEUNIVERSAL_AUTO_CONSOLE
        OpenConsoleUnlocked();
#endif
    }

    void Shutdown()
    {
        std::lock_guard lock(g_logMutex);
        if (!g_initialized)
            return;

        WriteUnlocked(L"FrostbiteUniversal unloading");
        g_initialized = false;
    }

    void Write(std::wstring_view message)
    {
        std::lock_guard lock(g_logMutex);
        if (g_logPath.empty())
            g_logPath = BuildLogPath();

        WriteUnlocked(message);
    }

    bool OpenConsole()
    {
        std::lock_guard lock(g_logMutex);
        return OpenConsoleUnlocked();
    }

    std::wstring GetLogPath()
    {
        std::lock_guard lock(g_logMutex);
        if (g_logPath.empty())
            g_logPath = BuildLogPath();

        return g_logPath.wstring();
    }

    bool CopyLogPath(wchar_t* outPath, std::uint32_t outPathLength)
    {
        if (!outPath || outPathLength == 0)
            return false;

        const std::wstring path = GetLogPath();
        wcsncpy_s(outPath, outPathLength, path.c_str(), _TRUNCATE);
        return true;
    }
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_OpenConsole()
{
    return FrostbiteUniversal::Log::OpenConsole() ? 1 : 0;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_GetLogPath(wchar_t* outPath, std::uint32_t outPathLength)
{
    return FrostbiteUniversal::Log::CopyLogPath(outPath, outPathLength) ? 1 : 0;
}
