#include "audio/AudioSessionScanner.h"

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <stdexcept>

namespace nexplay::audio {
namespace {

using Microsoft::WRL::ComPtr;

[[nodiscard]] AudioApplication processApplication(const DWORD processId) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) {
        return {processId, L"proces-" + std::to_wstring(processId), {}};
    }

    std::wstring path(32'768, L'\0');
    DWORD pathLength = static_cast<DWORD>(path.size());
    if (QueryFullProcessImageNameW(process, 0, path.data(), &pathLength) == FALSE) {
        CloseHandle(process);
        return {processId, L"proces-" + std::to_wstring(processId), {}};
    }
    CloseHandle(process);
    path.resize(pathLength);
    const std::filesystem::path executable(path);
    return {processId, executable.stem().wstring(), executable.filename().wstring()};
}

} // namespace

std::vector<AudioApplication> activeAudioApplications() {
    ComPtr<IMMDeviceEnumerator> deviceEnumerator;
    HRESULT result = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        IID_PPV_ARGS(&deviceEnumerator));
    if (FAILED(result)) {
        throw std::runtime_error("Nie mozna uruchomic enumeratora urzadzen audio.");
    }

    ComPtr<IMMDeviceCollection> devices;
    result = deviceEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices);
    if (FAILED(result)) {
        throw std::runtime_error("Nie mozna pobrac aktywnych wyjsc audio.");
    }

    UINT deviceCount{};
    devices->GetCount(&deviceCount);
    std::map<DWORD, AudioApplication> applications;

    for (UINT deviceIndex = 0; deviceIndex < deviceCount; ++deviceIndex) {
        ComPtr<IMMDevice> device;
        if (FAILED(devices->Item(deviceIndex, &device))) {
            continue;
        }

        ComPtr<IAudioSessionManager2> manager;
        if (FAILED(device->Activate(
                __uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &manager))) {
            continue;
        }

        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(manager->GetSessionEnumerator(&sessions))) {
            continue;
        }

        int sessionCount{};
        sessions->GetCount(&sessionCount);
        for (int sessionIndex = 0; sessionIndex < sessionCount; ++sessionIndex) {
            ComPtr<IAudioSessionControl> control;
            if (FAILED(sessions->GetSession(sessionIndex, &control))) {
                continue;
            }

            AudioSessionState state{};
            if (FAILED(control->GetState(&state)) || state != AudioSessionStateActive) {
                continue;
            }

            ComPtr<IAudioSessionControl2> control2;
            if (FAILED(control.As(&control2)) || control2->IsSystemSoundsSession() == S_OK) {
                continue;
            }

            DWORD processId{};
            if (FAILED(control2->GetProcessId(&processId)) || processId == 0 ||
                processId == GetCurrentProcessId()) {
                continue;
            }

            applications.try_emplace(processId, processApplication(processId));
        }
    }

    std::vector<AudioApplication> resultApplications;
    resultApplications.reserve(applications.size());
    for (auto& [processId, application] : applications) {
        resultApplications.push_back(std::move(application));
    }
    return resultApplications;
}

} // namespace nexplay::audio
