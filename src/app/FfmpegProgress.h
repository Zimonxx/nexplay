#pragma once
#include <Windows.h>
#include <functional>
#include <string>
#include <vector>

namespace nexplay::app {
DWORD runFfmpegProgress(const std::vector<std::wstring> &quotedArguments, double durationSeconds,
                        const std::function<void(int)> &progress);
}
