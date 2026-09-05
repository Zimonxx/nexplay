#include "encoding/NvencEncoder.h"

#include <algorithm>
#include <cstring>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace nexplay::encoding {
namespace {

using NvEncodeApiCreateInstanceFunction = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);

[[nodiscard]] bool equalGuid(const GUID& left, const GUID& right) {
    return std::memcmp(&left, &right, sizeof(GUID)) == 0;
}

} // namespace

NvencEncoder::NvencEncoder(
    ID3D11Device* device,
    ID3D11Texture2D* inputTexture,
    const NvencEncoderSettings settings)
    : settings_(settings) {
    if (device == nullptr || inputTexture == nullptr) {
        throw std::invalid_argument("NVENC otrzymal pusty zasob Direct3D 11.");
    }
    if (settings_.width == 0 || settings_.height == 0 || settings_.framesPerSecond == 0) {
        throw std::invalid_argument("Nieprawidlowe wymiary lub FPS dla NVENC.");
    }

    library_ = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (library_ == nullptr) {
        throw std::runtime_error("Nie znaleziono nvEncodeAPI64.dll.");
    }

    try {
        const auto createInstance = reinterpret_cast<NvEncodeApiCreateInstanceFunction>(
            GetProcAddress(library_, "NvEncodeAPICreateInstance"));
        if (createInstance == nullptr) {
            throw std::runtime_error("Sterownik nie udostepnia NvEncodeAPICreateInstance.");
        }

        api_ = {};
        api_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
        check(createInstance(&api_), "Utworzenie API NVENC");

        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS session{};
        session.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        session.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        session.device = device;
        session.apiVersion = NVENCAPI_VERSION;
        check(api_.nvEncOpenEncodeSessionEx(&session, &encoder_), "Otwarcie sesji NVENC");

        std::uint32_t codecCount{};
        check(api_.nvEncGetEncodeGUIDCount(encoder_, &codecCount), "Odczytanie listy kodekow NVENC");
        std::vector<GUID> codecs(codecCount);
        check(api_.nvEncGetEncodeGUIDs(
                  encoder_, codecs.data(), static_cast<std::uint32_t>(codecs.size()), &codecCount),
              "Odczytanie kodekow NVENC");
        if (std::none_of(codecs.begin(), codecs.begin() + codecCount, [](const GUID& codec) {
                return equalGuid(codec, NV_ENC_CODEC_H264_GUID);
            })) {
            throw std::runtime_error("Karta nie obsluguje kodowania H.264 przez NVENC.");
        }

        std::uint32_t formatCount{};
        check(api_.nvEncGetInputFormatCount(encoder_, NV_ENC_CODEC_H264_GUID, &formatCount),
              "Odczytanie liczby formatow NVENC");
        std::vector<NV_ENC_BUFFER_FORMAT> formats(formatCount);
        check(api_.nvEncGetInputFormats(
                  encoder_, NV_ENC_CODEC_H264_GUID, formats.data(),
                  static_cast<std::uint32_t>(formats.size()), &formatCount),
              "Odczytanie formatow NVENC");
        if (std::find(formats.begin(), formats.begin() + formatCount, NV_ENC_BUFFER_FORMAT_ARGB) ==
            formats.begin() + formatCount) {
            throw std::runtime_error("Karta nie obsluguje tekstury BGRA jako wejscia H.264 NVENC.");
        }

        NV_ENC_PRESET_CONFIG preset{};
        preset.version = NV_ENC_PRESET_CONFIG_VER;
        preset.presetCfg.version = NV_ENC_CONFIG_VER;
        check(api_.nvEncGetEncodePresetConfigEx(
                  encoder_, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P4_GUID,
                  NV_ENC_TUNING_INFO_LOW_LATENCY, &preset),
              "Pobranie ustawien presetu NVENC");

        NV_ENC_CONFIG configuration = preset.presetCfg;
        configuration.version = NV_ENC_CONFIG_VER;
        configuration.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
        configuration.gopLength = settings_.framesPerSecond * 2U;
        configuration.frameIntervalP = 1;
        configuration.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        configuration.rcParams.averageBitRate = settings_.bitrate;
        configuration.rcParams.maxBitRate = settings_.bitrate;
        configuration.encodeCodecConfig.h264Config.idrPeriod = configuration.gopLength;
        configuration.encodeCodecConfig.h264Config.repeatSPSPPS = 1;

        NV_ENC_INITIALIZE_PARAMS initialize{};
        initialize.version = NV_ENC_INITIALIZE_PARAMS_VER;
        initialize.encodeGUID = NV_ENC_CODEC_H264_GUID;
        initialize.presetGUID = NV_ENC_PRESET_P4_GUID;
        initialize.encodeWidth = settings_.width;
        initialize.encodeHeight = settings_.height;
        initialize.darWidth = settings_.width;
        initialize.darHeight = settings_.height;
        initialize.frameRateNum = settings_.framesPerSecond;
        initialize.frameRateDen = 1;
        initialize.enableEncodeAsync = 0;
        initialize.enablePTD = 1;
        initialize.encodeConfig = &configuration;
        initialize.maxEncodeWidth = settings_.width;
        initialize.maxEncodeHeight = settings_.height;
        initialize.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
        check(api_.nvEncInitializeEncoder(encoder_, &initialize), "Inicjalizacja kodera NVENC");

        NV_ENC_CREATE_BITSTREAM_BUFFER bitstream{};
        bitstream.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        check(api_.nvEncCreateBitstreamBuffer(encoder_, &bitstream),
              "Utworzenie bufora wyjsciowego NVENC");
        bitstreamBuffer_ = bitstream.bitstreamBuffer;

        NV_ENC_REGISTER_RESOURCE resource{};
        resource.version = NV_ENC_REGISTER_RESOURCE_VER;
        resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        resource.width = settings_.width;
        resource.height = settings_.height;
        resource.pitch = 0;
        resource.subResourceIndex = 0;
        resource.resourceToRegister = inputTexture;
        resource.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
        resource.bufferUsage = NV_ENC_INPUT_IMAGE;
        check(api_.nvEncRegisterResource(encoder_, &resource),
              "Rejestracja tekstury Direct3D w NVENC");
        registeredInput_ = resource.registeredResource;

        NV_ENC_MAP_INPUT_RESOURCE mapping{};
        mapping.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        mapping.registeredResource = registeredInput_;
        check(api_.nvEncMapInputResource(encoder_, &mapping), "Mapowanie tekstury NVENC");
        mappedInput_ = mapping.mappedResource;
    } catch (...) {
        close();
        throw;
    }
}

NvencEncoder::~NvencEncoder() {
    close();
}

void NvencEncoder::encodeFrame(
    std::ostream& output,
    const std::uint64_t frameIndex,
    const bool forceKeyFrame) {
    if (finished_) {
        throw std::logic_error("Nie mozna kodowac po zakonczeniu sesji NVENC.");
    }

    NV_ENC_PIC_PARAMS picture{};
    picture.version = NV_ENC_PIC_PARAMS_VER;
    picture.inputWidth = settings_.width;
    picture.inputHeight = settings_.height;
    picture.inputPitch = 0;
    if (forceKeyFrame) {
        picture.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    }
    picture.frameIdx = static_cast<std::uint32_t>(frameIndex);
    picture.inputTimeStamp = frameIndex;
    picture.inputDuration = 1;
    picture.inputBuffer = mappedInput_;
    picture.outputBitstream = bitstreamBuffer_;
    picture.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;
    picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    check(api_.nvEncEncodePicture(encoder_, &picture), "Kodowanie klatki NVENC");

    NV_ENC_LOCK_BITSTREAM lock{};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = bitstreamBuffer_;
    check(api_.nvEncLockBitstream(encoder_, &lock), "Pobranie strumienia H.264 z NVENC");

    output.write(
        static_cast<const char*>(lock.bitstreamBufferPtr),
        static_cast<std::streamsize>(lock.bitstreamSizeInBytes));
    const bool writeFailed = !output.good();
    check(api_.nvEncUnlockBitstream(encoder_, bitstreamBuffer_), "Zwolnienie strumienia NVENC");

    if (writeFailed) {
        throw std::runtime_error("Nie udalo sie zapisac danych H.264 do pliku.");
    }
}

void NvencEncoder::finish() {
    if (finished_) {
        return;
    }

    NV_ENC_PIC_PARAMS endOfStream{};
    endOfStream.version = NV_ENC_PIC_PARAMS_VER;
    endOfStream.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    check(api_.nvEncEncodePicture(encoder_, &endOfStream), "Zakonczenie strumienia NVENC");
    finished_ = true;
}

void NvencEncoder::check(const NVENCSTATUS status, const char* operation) const {
    if (status == NV_ENC_SUCCESS) {
        return;
    }

    std::ostringstream message;
    message << operation << " nie powiodlo sie (NVENC " << static_cast<int>(status) << ')';
    if (encoder_ != nullptr && api_.nvEncGetLastErrorString != nullptr) {
        if (const char* details = api_.nvEncGetLastErrorString(encoder_); details != nullptr) {
            message << ": " << details;
        }
    }
    throw std::runtime_error(message.str());
}

void NvencEncoder::close() noexcept {
    if (encoder_ != nullptr) {
        if (mappedInput_ != nullptr && api_.nvEncUnmapInputResource != nullptr) {
            api_.nvEncUnmapInputResource(encoder_, mappedInput_);
            mappedInput_ = nullptr;
        }
        if (registeredInput_ != nullptr && api_.nvEncUnregisterResource != nullptr) {
            api_.nvEncUnregisterResource(encoder_, registeredInput_);
            registeredInput_ = nullptr;
        }
        if (bitstreamBuffer_ != nullptr && api_.nvEncDestroyBitstreamBuffer != nullptr) {
            api_.nvEncDestroyBitstreamBuffer(encoder_, bitstreamBuffer_);
            bitstreamBuffer_ = nullptr;
        }
        if (api_.nvEncDestroyEncoder != nullptr) {
            api_.nvEncDestroyEncoder(encoder_);
        }
        encoder_ = nullptr;
    }
    if (library_ != nullptr) {
        FreeLibrary(library_);
        library_ = nullptr;
    }
}

} // namespace nexplay::encoding
