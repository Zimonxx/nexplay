#pragma once
#include <Windows.h>
#include <shellapi.h>
#include <string>

namespace nexplay::platform {
inline bool isInstallationVerificationRequest(const wchar_t* arguments) {
    if (!arguments) return false;
    // wWinMain receives the raw tail, including PowerShell 5.1's trailing
    // space. Parse tokens instead of comparing that raw string literally.
    const std::wstring command = std::wstring(L"nexplay.exe ") + arguments;
    int count{};
    auto tokens = CommandLineToArgvW(command.c_str(), &count);
    if (!tokens) return false;
    const bool verification = count == 2 && wcscmp(tokens[1], L"--verify-installation") == 0;
    LocalFree(tokens);
    return verification;
}
}
