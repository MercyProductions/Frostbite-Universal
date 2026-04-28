#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace FrostbiteUniversal::Log
{
    void Initialize(HMODULE selfModule);
    void Shutdown();
    void Write(std::wstring_view message);
    bool OpenConsole();
    std::wstring GetLogPath();
    bool CopyLogPath(wchar_t* outPath, std::uint32_t outPathLength);
}
