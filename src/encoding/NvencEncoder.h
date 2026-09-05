#pragma once

#include <Windows.h>

#include <cstdint>
#include <iosfwd>

#include <d3d11.h>
#include <nvEncodeAPI.h>

namespace nexplay::encoding {

struct NvencEncoderSettings {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t framesPerSecond{60};
    std::uint32_t bitrate{25'000'000};
};

class NvencEncoder final {
public:
    NvencEncoder(
        ID3D11Device* device,
        ID3D11Texture2D* inputTexture,
        NvencEncoderSettings settings);
    ~NvencEncoder();

    NvencEncoder(const NvencEncoder&) = delete;
    NvencEncoder& operator=(const NvencEncoder&) = delete;

    void encodeFrame(
        std::ostream& output,
        std::uint64_t frameIndex,
        bool forceKeyFrame = false);
    void finish();

private:
    void check(NVENCSTATUS status, const char* operation) const;
    void close() noexcept;

    HMODULE library_{};
    NV_ENCODE_API_FUNCTION_LIST api_{};
    void* encoder_{};
    NV_ENC_OUTPUT_PTR bitstreamBuffer_{};
    NV_ENC_REGISTERED_PTR registeredInput_{};
    NV_ENC_INPUT_PTR mappedInput_{};
    NvencEncoderSettings settings_{};
    bool finished_{};
};

} // namespace nexplay::encoding
