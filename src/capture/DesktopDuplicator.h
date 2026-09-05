#pragma once

#include <cstdint>
#include <string>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace nexplay::capture {

class DesktopDuplicator final {
public:
    explicit DesktopDuplicator(
        std::uint32_t outputWidth = 0,
        std::uint32_t outputHeight = 0);

    DesktopDuplicator(const DesktopDuplicator&) = delete;
    DesktopDuplicator& operator=(const DesktopDuplicator&) = delete;

    [[nodiscard]] bool acquireLatestFrame(std::uint32_t timeoutMilliseconds);

    [[nodiscard]] ID3D11Device* device() const noexcept;
    [[nodiscard]] ID3D11Texture2D* frameTexture() const noexcept;
    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    [[nodiscard]] std::uint32_t adapterVendorId() const noexcept;
    [[nodiscard]] const std::wstring& adapterName() const noexcept;

private:
    void orientFrame();

    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> sourceTexture_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> frameTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sourceView_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> frameTarget_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> orientationVertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> orientationPixelShader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> orientationConstants_;
    DXGI_MODE_ROTATION rotation_{DXGI_MODE_ROTATION_IDENTITY};
    std::uint32_t sourceWidth_{};
    std::uint32_t sourceHeight_{};
    std::uint32_t width_{};
    std::uint32_t height_{};
    std::uint32_t adapterVendorId_{};
    std::wstring adapterName_;
};

} // namespace nexplay::capture
