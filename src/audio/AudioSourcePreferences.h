#pragma once
#include <Windows.h>
#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace nexplay::audio {
struct ApplicationNameLess {
    bool operator()(const std::wstring& left, const std::wstring& right) const noexcept {
        return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    }
};
using ExcludedApplications = std::set<std::wstring, ApplicationNameLess>;

inline bool isApplicationExcluded(const ExcludedApplications& excluded, const std::wstring& id) {
    return !id.empty() && excluded.contains(id);
}

inline std::vector<wchar_t> encodeExclusions(const ExcludedApplications& excluded) {
    std::vector<wchar_t> data;
    for (const auto& id : excluded) {
        if (id.empty()) continue;
        data.insert(data.end(), id.begin(), id.end());
        data.push_back(L'\0');
    }
    if (data.empty()) data.push_back(L'\0');
    data.push_back(L'\0');
    return data;
}

inline ExcludedApplications decodeExclusions(const std::vector<wchar_t>& data) {
    ExcludedApplications excluded;
    if (data.size() < 2 || data.back() != L'\0' || data[data.size()-2] != L'\0') return excluded;
    for (std::size_t start = 0; start < data.size() && data[start];) {
        auto end = std::find(data.begin() + start, data.end(), L'\0');
        excluded.emplace(data.begin() + start, end);
        start = static_cast<std::size_t>(end - data.begin()) + 1;
    }
    return excluded;
}

inline ExcludedApplications loadAudioExclusions(HKEY root, const wchar_t* path) {
    DWORD bytes{};
    if (RegGetValueW(root, path, L"ExcludedAudioApplications", RRF_RT_REG_MULTI_SZ,
            nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < 2 * sizeof(wchar_t) ||
            bytes > 1024 * 1024 || bytes % sizeof(wchar_t)) return {};
    std::vector<wchar_t> data(bytes / sizeof(wchar_t));
    if (RegGetValueW(root, path, L"ExcludedAudioApplications", RRF_RT_REG_MULTI_SZ,
            nullptr, data.data(), &bytes) != ERROR_SUCCESS) return {};
    data.resize(bytes / sizeof(wchar_t));
    return decodeExclusions(data);
}

inline bool saveAudioExclusions(HKEY root, const wchar_t* path, const ExcludedApplications& excluded) {
    const auto data = encodeExclusions(excluded);
    return RegSetKeyValueW(root, path, L"ExcludedAudioApplications", REG_MULTI_SZ,
        data.data(), static_cast<DWORD>(data.size() * sizeof(wchar_t))) == ERROR_SUCCESS;
}

// Move complete rows, retaining process/group metadata and the order inside
// each section. Shared by refresh and immediate checkbox changes.
template<class Rows> void sortAudioSources(Rows& rows) {
    std::stable_partition(rows.begin(), rows.end(), [](const auto& row) { return row.included; });
}
}
