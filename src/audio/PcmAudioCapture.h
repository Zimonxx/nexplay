#pragma once

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <span>
#include <thread>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

namespace nexplay::audio {

struct PcmFormat {
    std::uint32_t sampleRate{};
    std::uint16_t channels{};
    std::uint16_t bitsPerSample{};
    std::uint16_t blockAlign{};
};

using PcmSink = std::function<void(PcmFormat, std::span<const std::byte>, std::uint32_t)>;

class PcmAudioCapture final {
public:
    PcmAudioCapture();
    ~PcmAudioCapture();

    PcmAudioCapture(const PcmAudioCapture&) = delete;
    PcmAudioCapture& operator=(const PcmAudioCapture&) = delete;

    void startProcess(DWORD processId, const std::filesystem::path& outputPath);
    void startDefaultMicrophone(const std::filesystem::path& outputPath);
    void startProcessToSink(DWORD processId, PcmSink sink);
    void startDefaultMicrophoneToSink(PcmSink sink);
    void stop();

private:
    void prepareProcess(DWORD processId);
    void prepareDefaultMicrophone();
    void initializeClient(bool processLoopback, const WAVEFORMATEX* requestedFormat = nullptr);
    void startWriter(const std::filesystem::path& outputPath);
    void startSink(PcmSink sink);
    void startEngine();
    void captureLoop(std::stop_token stopToken);
    void drainPackets();
    void writeWavHeader();
    void finalizeWavHeader();
    void stopNoThrow() noexcept;

    Microsoft::WRL::ComPtr<IAudioClient> audioClient_;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> captureClient_;
    HANDLE samplesReadyEvent_{};
    WAVEFORMATEXTENSIBLE captureFormat_{};
    std::uint32_t formatChunkSize_{16};
    std::ofstream output_;
    PcmSink sink_;
    std::jthread captureThread_;
    std::exception_ptr captureError_;
    std::uint64_t audioBytes_{};
    bool eventDriven_{};
    bool started_{};
};

} // namespace nexplay::audio
