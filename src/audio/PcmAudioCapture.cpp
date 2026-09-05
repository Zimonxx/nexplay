#include "audio/PcmAudioCapture.h"

#include <audioclientactivationparams.h>
#include <wrl/implements.h>

#include <array>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace nexplay::audio {
namespace {

using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::FtmBase;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

class ActivationHandler final : public RuntimeClass<
    RuntimeClassFlags<ClassicCom>,
    FtmBase,
    IActivateAudioInterfaceCompletionHandler> {
public:
    ActivationHandler()
        : completed_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
    }

    ~ActivationHandler() override {
        if (completed_ != nullptr) {
            CloseHandle(completed_);
        }
    }

    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        ComPtr<IUnknown> activated;
        HRESULT activationResult = E_FAIL;
        result_ = operation->GetActivateResult(&activationResult, &activated);
        if (SUCCEEDED(result_)) {
            result_ = activationResult;
        }
        if (SUCCEEDED(result_)) {
            result_ = activated.As(&client_);
        }
        SetEvent(completed_);
        return S_OK;
    }

    [[nodiscard]] HANDLE completedEvent() const noexcept {
        return completed_;
    }

    [[nodiscard]] HRESULT result() const noexcept {
        return result_;
    }

    [[nodiscard]] ComPtr<IAudioClient> client() const {
        return client_;
    }

private:
    HANDLE completed_{};
    HRESULT result_{E_PENDING};
    ComPtr<IAudioClient> client_;
};

void writeFourCc(std::ostream& output, const char (&text)[5]) {
    output.write(text, 4);
}

void writeUInt16(std::ostream& output, const std::uint16_t value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void writeUInt32(std::ostream& output, const std::uint32_t value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

[[nodiscard]] std::runtime_error audioClientError(
    const char* message,
    const HRESULT result) {
    std::ostringstream text;
    text << message << " (HRESULT 0x" << std::hex << std::uppercase
         << static_cast<unsigned long>(result) << ").";
    return std::runtime_error(text.str());
}

} // namespace

PcmAudioCapture::PcmAudioCapture() {
    captureFormat_.Format.wFormatTag = WAVE_FORMAT_PCM;
    captureFormat_.Format.nChannels = 2;
    captureFormat_.Format.nSamplesPerSec = 48'000;
    captureFormat_.Format.wBitsPerSample = 16;
    captureFormat_.Format.nBlockAlign =
        captureFormat_.Format.nChannels * captureFormat_.Format.wBitsPerSample / 8;
    captureFormat_.Format.nAvgBytesPerSec =
        captureFormat_.Format.nSamplesPerSec * captureFormat_.Format.nBlockAlign;
}

PcmAudioCapture::~PcmAudioCapture() {
    stopNoThrow();
    if (samplesReadyEvent_ != nullptr) {
        CloseHandle(samplesReadyEvent_);
    }
}

void PcmAudioCapture::startProcess(
    const DWORD processId,
    const std::filesystem::path& outputPath) {
    prepareProcess(processId);
    startWriter(outputPath);
}

void PcmAudioCapture::startProcessToSink(const DWORD processId, PcmSink sink) {
    prepareProcess(processId);
    startSink(std::move(sink));
}

void PcmAudioCapture::prepareProcess(const DWORD processId) {
    if (started_) {
        throw std::logic_error("Przechwytywanie audio jest juz uruchomione.");
    }

    const ComPtr<ActivationHandler> handler = Make<ActivationHandler>();
    if (handler == nullptr || handler->completedEvent() == nullptr) {
        throw std::runtime_error("Nie mozna utworzyc obslugi aktywacji audio procesu.");
    }

    AUDIOCLIENT_ACTIVATION_PARAMS parameters{};
    parameters.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    parameters.ProcessLoopbackParams.TargetProcessId = processId;
    parameters.ProcessLoopbackParams.ProcessLoopbackMode =
        PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activationParameters{};
    activationParameters.vt = VT_BLOB;
    activationParameters.blob.cbSize = sizeof(parameters);
    activationParameters.blob.pBlobData = reinterpret_cast<BYTE*>(&parameters);

    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    const HRESULT activationStart = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
        __uuidof(IAudioClient),
        &activationParameters,
        handler.Get(),
        &operation);
    if (FAILED(activationStart)) {
        throw std::runtime_error("Nie mozna rozpoczac aktywacji audio procesu.");
    }
    if (WaitForSingleObject(handler->completedEvent(), 5'000) != WAIT_OBJECT_0) {
        throw std::runtime_error("Aktywacja audio procesu przekroczyla limit czasu.");
    }
    if (FAILED(handler->result())) {
        throw std::runtime_error("Windows odmowil przechwytywania audio procesu.");
    }

    audioClient_ = handler->client();
    initializeClient(true);
}

void PcmAudioCapture::startDefaultMicrophone(const std::filesystem::path& outputPath) {
    prepareDefaultMicrophone();
    startWriter(outputPath);
}

void PcmAudioCapture::startDefaultMicrophoneToSink(PcmSink sink) {
    prepareDefaultMicrophone();
    startSink(std::move(sink));
}

void PcmAudioCapture::prepareDefaultMicrophone() {
    if (started_) {
        throw std::logic_error("Przechwytywanie audio jest juz uruchomione.");
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT result = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(result)) {
        throw std::runtime_error("Nie mozna uruchomic enumeratora mikrofonow.");
    }

    ComPtr<IMMDevice> microphone;
    result = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &microphone);
    if (FAILED(result)) {
        throw std::runtime_error("Nie znaleziono domyslnego mikrofonu.");
    }
    result = microphone->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &audioClient_);
    if (FAILED(result)) {
        throw std::runtime_error("Nie mozna uruchomic domyslnego mikrofonu.");
    }

    WAVEFORMATEX* microphoneFormat{};
    result = audioClient_->GetMixFormat(&microphoneFormat);
    if (FAILED(result) || microphoneFormat == nullptr) {
        throw std::runtime_error("Nie mozna odczytac formatu domyslnego mikrofonu.");
    }
    const WORD microphoneChannels = microphoneFormat->nChannels;
    const DWORD microphoneSampleRate = microphoneFormat->nSamplesPerSec;
    CoTaskMemFree(microphoneFormat);

    captureFormat_ = {};
    captureFormat_.Format.wFormatTag = WAVE_FORMAT_PCM;
    captureFormat_.Format.nChannels = microphoneChannels;
    captureFormat_.Format.nSamplesPerSec = microphoneSampleRate;
    captureFormat_.Format.wBitsPerSample = 16;
    captureFormat_.Format.nBlockAlign =
        captureFormat_.Format.nChannels * captureFormat_.Format.wBitsPerSample / 8;
    captureFormat_.Format.nAvgBytesPerSec =
        captureFormat_.Format.nSamplesPerSec * captureFormat_.Format.nBlockAlign;
    formatChunkSize_ = 16;

    initializeClient(false);
}

void PcmAudioCapture::initializeClient(
    const bool processLoopback,
    const WAVEFORMATEX* requestedFormat) {
    samplesReadyEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (samplesReadyEvent_ == nullptr) {
        throw std::runtime_error("Nie mozna utworzyc zdarzenia audio.");
    }

    DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                  AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    eventDriven_ = processLoopback;
    if (processLoopback) {
        flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                 AUDCLNT_STREAMFLAGS_LOOPBACK |
                 AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                 AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    }

    constexpr REFERENCE_TIME oneSecond = 10'000'000;
    const REFERENCE_TIME bufferDuration = processLoopback ? 0 : oneSecond;
    HRESULT result = audioClient_->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        flags,
        bufferDuration,
        0,
        requestedFormat != nullptr ? requestedFormat : &captureFormat_.Format,
        nullptr);
    if (FAILED(result)) {
        std::ostringstream formatDetails;
        formatDetails << "Nie mozna ustawic formatu przechwytywania audio"
                      << " [tag=" << captureFormat_.Format.wFormatTag
                      << ", channels=" << captureFormat_.Format.nChannels
                      << ", rate=" << captureFormat_.Format.nSamplesPerSec
                      << ", bits=" << captureFormat_.Format.wBitsPerSample
                      << ", block=" << captureFormat_.Format.nBlockAlign
                      << ", cbSize=" << captureFormat_.Format.cbSize << ']';
        throw audioClientError(formatDetails.str().c_str(), result);
    }
    result = audioClient_->GetService(IID_PPV_ARGS(&captureClient_));
    if (FAILED(result)) {
        throw std::runtime_error("Nie mozna pobrac bufora przechwytywania audio.");
    }
    if (eventDriven_) {
        result = audioClient_->SetEventHandle(samplesReadyEvent_);
        if (FAILED(result)) {
            throw std::runtime_error("Nie mozna ustawic zdarzenia probek audio.");
        }
    }
}

void PcmAudioCapture::startWriter(const std::filesystem::path& outputPath) {
    output_.open(outputPath, std::ios::binary | std::ios::trunc);
    if (!output_) {
        throw std::runtime_error("Nie mozna utworzyc pliku WAV.");
    }
    writeWavHeader();

    startEngine();
}

void PcmAudioCapture::startSink(PcmSink sink) {
    if (!sink) {
        throw std::invalid_argument("Nie ustawiono odbiorcy probek audio.");
    }
    sink_ = std::move(sink);
    startEngine();
}

void PcmAudioCapture::startEngine() {
    const HRESULT result = audioClient_->Start();
    if (FAILED(result)) {
        throw std::runtime_error("Nie mozna uruchomic przechwytywania audio.");
    }
    started_ = true;
    captureThread_ = std::jthread([this](const std::stop_token stopToken) {
        captureLoop(stopToken);
    });
}

void PcmAudioCapture::stop() {
    if (!started_) {
        return;
    }
    captureThread_.request_stop();
    SetEvent(samplesReadyEvent_);
    captureThread_.join();
    audioClient_->Stop();
    started_ = false;
    finalizeWavHeader();
    output_.close();

    if (captureError_ != nullptr) {
        std::rethrow_exception(captureError_);
    }
}

void PcmAudioCapture::captureLoop(const std::stop_token stopToken) {
    try {
        while (!stopToken.stop_requested()) {
            const DWORD waitTime = eventDriven_ ? 100 : 10;
            const DWORD waitResult = WaitForSingleObject(samplesReadyEvent_, waitTime);
            if (eventDriven_ ? waitResult == WAIT_OBJECT_0 : waitResult == WAIT_TIMEOUT) {
                drainPackets();
            }
        }
        drainPackets();
    } catch (...) {
        captureError_ = std::current_exception();
    }
}

void PcmAudioCapture::drainPackets() {
    UINT32 availableFrames{};
    while (SUCCEEDED(captureClient_->GetNextPacketSize(&availableFrames)) &&
           availableFrames > 0) {
        BYTE* data{};
        DWORD flags{};
        UINT64 devicePosition{};
        UINT64 qpcPosition{};
        const HRESULT result = captureClient_->GetBuffer(
            &data,
            &availableFrames,
            &flags,
            &devicePosition,
            &qpcPosition);
        if (FAILED(result)) {
            throw std::runtime_error("Nie mozna odczytac probek audio.");
        }

        const std::size_t bytes =
            static_cast<std::size_t>(availableFrames) * captureFormat_.Format.nBlockAlign;
        std::vector<std::byte> silence;
        const std::byte* samples = reinterpret_cast<const std::byte*>(data);
        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
            silence.resize(bytes);
            samples = silence.data();
        }

        if (output_.is_open()) {
            output_.write(
                reinterpret_cast<const char*>(samples),
                static_cast<std::streamsize>(bytes));
        }
        if (sink_) {
            const PcmFormat format{
                .sampleRate = captureFormat_.Format.nSamplesPerSec,
                .channels = captureFormat_.Format.nChannels,
                .bitsPerSample = captureFormat_.Format.wBitsPerSample,
                .blockAlign = captureFormat_.Format.nBlockAlign,
            };
            sink_(format, std::span<const std::byte>(samples, bytes), availableFrames);
        }
        captureClient_->ReleaseBuffer(availableFrames);
        if (output_.is_open() && !output_) {
            throw std::runtime_error("Nie mozna zapisac probek audio.");
        }
        audioBytes_ += bytes;
    }
}

void PcmAudioCapture::writeWavHeader() {
    writeFourCc(output_, "RIFF");
    writeUInt32(output_, 0);
    writeFourCc(output_, "WAVE");
    writeFourCc(output_, "fmt ");
    writeUInt32(output_, formatChunkSize_);
    output_.write(
        reinterpret_cast<const char*>(&captureFormat_),
        static_cast<std::streamsize>(formatChunkSize_));
    writeFourCc(output_, "data");
    writeUInt32(output_, 0);
}

void PcmAudioCapture::finalizeWavHeader() {
    if (!output_.is_open()) {
        return;
    }
    const auto dataSize = static_cast<std::uint32_t>(audioBytes_);
    const auto riffSize = static_cast<std::uint32_t>(20 + formatChunkSize_ + audioBytes_);
    output_.seekp(4, std::ios::beg);
    writeUInt32(output_, riffSize);
    output_.seekp(24 + formatChunkSize_, std::ios::beg);
    writeUInt32(output_, dataSize);
    output_.flush();
}

void PcmAudioCapture::stopNoThrow() noexcept {
    try {
        stop();
    } catch (...) {
    }
}

} // namespace nexplay::audio
