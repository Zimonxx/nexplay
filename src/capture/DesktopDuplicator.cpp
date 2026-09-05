#include "capture/DesktopDuplicator.h"

#include <Windows.h>
#include <d3dcompiler.h>

#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace nexplay::capture {
namespace {

[[nodiscard]] std::runtime_error hresultError(const char* operation, const HRESULT result) {
    std::ostringstream message;
    message << operation << " nie powiodlo sie (HRESULT 0x"
            << std::hex << std::uppercase << static_cast<unsigned long>(result) << ").";
    return std::runtime_error(message.str());
}

[[nodiscard]] bool isPrimaryMonitor(const HMONITOR monitor) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    return GetMonitorInfoW(monitor, &info) != FALSE &&
           (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
}

[[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob> compileShader(
    const std::string_view source,
    const char* entryPoint,
    const char* target) {
    Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(
        source.data(),
        source.size(),
        nullptr,
        nullptr,
        nullptr,
        entryPoint,
        target,
        D3DCOMPILE_ENABLE_STRICTNESS,
        0,
        &bytecode,
        &errors);
    if (FAILED(result)) {
        std::string details;
        if (errors != nullptr) {
            details.assign(
                static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize());
        }
        throw std::runtime_error("Kompilacja shadera obrotu nie powiodla sie: " + details);
    }
    return bytecode;
}

constexpr std::string_view orientationVertexShader = R"hlsl(
float4 main(uint vertexId : SV_VertexID) : SV_Position
{
    float2 position = float2((vertexId << 1) & 2, vertexId & 2);
    return float4(position * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)hlsl";

constexpr std::string_view orientationPixelShader = R"hlsl(
Texture2D<float4> sourceFrame : register(t0);

cbuffer OrientationConstants : register(b0)
{
    uint sourceWidth;
    uint sourceHeight;
    uint rotation;
    uint padding;
};

float4 main(float4 position : SV_Position) : SV_Target
{
    uint2 destination = uint2(position.xy);
    uint2 source;

    if (rotation == 1)
    {
        source = uint2(sourceWidth - 1 - destination.y, destination.x);
    }
    else if (rotation == 2)
    {
        source = uint2(sourceWidth - 1 - destination.x, sourceHeight - 1 - destination.y);
    }
    else if (rotation == 3)
    {
        source = uint2(destination.y, sourceHeight - 1 - destination.x);
    }
    else
    {
        source = destination;
    }

    return sourceFrame.Load(int3(source, 0));
}
)hlsl";

struct OrientationConstants {
    std::uint32_t sourceWidth;
    std::uint32_t sourceHeight;
    std::uint32_t rotation;
    std::uint32_t padding{};
};

[[nodiscard]] std::uint32_t shaderRotation(const DXGI_MODE_ROTATION rotation) {
    switch (rotation) {
    case DXGI_MODE_ROTATION_ROTATE90:
        return 1;
    case DXGI_MODE_ROTATION_ROTATE180:
        return 2;
    case DXGI_MODE_ROTATION_ROTATE270:
        return 3;
    case DXGI_MODE_ROTATION_UNSPECIFIED:
    case DXGI_MODE_ROTATION_IDENTITY:
    default:
        return 0;
    }
}

} // namespace

DesktopDuplicator::DesktopDuplicator(
    const std::uint32_t outputWidth,
    const std::uint32_t outputHeight) {
    if ((outputWidth == 0) != (outputHeight == 0) ||
        outputWidth > 7'680 || outputHeight > 7'680 ||
        static_cast<std::uint64_t>(outputWidth) * outputHeight >
            static_cast<std::uint64_t>(7'680) * 4'320 ||
        (outputWidth != 0 && ((outputWidth % 2) != 0 || (outputHeight % 2) != 0))) {
        throw std::invalid_argument("Nieprawidlowa rozdzielczosc wyjsciowa.");
    }
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        throw hresultError("Utworzenie fabryki DXGI", result);
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter1> selectedAdapter;
    Microsoft::WRL::ComPtr<IDXGIOutput> selectedOutput;

    for (UINT adapterIndex = 0; ; ++adapterIndex) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }

        for (UINT outputIndex = 0; ; ++outputIndex) {
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(outputIndex, &output) == DXGI_ERROR_NOT_FOUND) {
                break;
            }

            DXGI_OUTPUT_DESC outputDescription{};
            if (SUCCEEDED(output->GetDesc(&outputDescription)) &&
                isPrimaryMonitor(outputDescription.Monitor)) {
                selectedAdapter = adapter;
                selectedOutput = output;
                break;
            }
        }

        if (selectedOutput != nullptr) {
            break;
        }
    }

    if (selectedAdapter == nullptr || selectedOutput == nullptr) {
        throw std::runtime_error("Nie znaleziono glownego monitora w DXGI.");
    }

    DXGI_ADAPTER_DESC1 adapterDescription{};
    result = selectedAdapter->GetDesc1(&adapterDescription);
    if (FAILED(result)) {
        throw hresultError("Odczytanie informacji o karcie graficznej", result);
    }
    adapterVendorId_ = adapterDescription.VendorId;
    adapterName_ = adapterDescription.Description;

    constexpr D3D_FEATURE_LEVEL requestedLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL selectedLevel{};
    result = D3D11CreateDevice(
        selectedAdapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        requestedLevels,
        static_cast<UINT>(std::size(requestedLevels)),
        D3D11_SDK_VERSION,
        &device_,
        &selectedLevel,
        &context_);
    if (FAILED(result)) {
        throw hresultError("Utworzenie urzadzenia Direct3D 11", result);
    }

    Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
    result = selectedOutput.As(&output1);
    if (FAILED(result)) {
        throw hresultError("Pobranie interfejsu IDXGIOutput1", result);
    }

    result = output1->DuplicateOutput(device_.Get(), &duplication_);
    if (FAILED(result)) {
        throw hresultError("Uruchomienie przechwytywania glownego monitora", result);
    }

    DXGI_OUTDUPL_DESC duplicationDescription{};
    duplication_->GetDesc(&duplicationDescription);
    if (duplicationDescription.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        throw std::runtime_error("Glowny monitor zwrocil nieobslugiwany format pikseli.");
    }

    rotation_ = duplicationDescription.Rotation;
    sourceWidth_ = duplicationDescription.ModeDesc.Width;
    sourceHeight_ = duplicationDescription.ModeDesc.Height;
    const bool swapsDimensions = rotation_ == DXGI_MODE_ROTATION_ROTATE90 ||
                                 rotation_ == DXGI_MODE_ROTATION_ROTATE270;
    const std::uint32_t nativeWidth = swapsDimensions ? sourceHeight_ : sourceWidth_;
    const std::uint32_t nativeHeight = swapsDimensions ? sourceWidth_ : sourceHeight_;
    width_ = outputWidth == 0 ? nativeWidth : outputWidth;
    height_ = outputHeight == 0 ? nativeHeight : outputHeight;

    D3D11_TEXTURE2D_DESC sourceDescription{};
    sourceDescription.Width = sourceWidth_;
    sourceDescription.Height = sourceHeight_;
    sourceDescription.MipLevels = 1;
    sourceDescription.ArraySize = 1;
    sourceDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sourceDescription.SampleDesc.Count = 1;
    sourceDescription.Usage = D3D11_USAGE_DEFAULT;
    sourceDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    result = device_->CreateTexture2D(&sourceDescription, nullptr, &sourceTexture_);
    if (FAILED(result)) {
        throw hresultError("Utworzenie tekstury zrodlowej", result);
    }
    result = device_->CreateShaderResourceView(sourceTexture_.Get(), nullptr, &sourceView_);
    if (FAILED(result)) {
        throw hresultError("Utworzenie widoku tekstury zrodlowej", result);
    }

    D3D11_TEXTURE2D_DESC textureDescription{};
    textureDescription.Width = width_;
    textureDescription.Height = height_;
    textureDescription.MipLevels = 1;
    textureDescription.ArraySize = 1;
    textureDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    textureDescription.SampleDesc.Count = 1;
    textureDescription.Usage = D3D11_USAGE_DEFAULT;
    textureDescription.BindFlags = D3D11_BIND_RENDER_TARGET;

    result = device_->CreateTexture2D(&textureDescription, nullptr, &frameTexture_);
    if (FAILED(result)) {
        throw hresultError("Utworzenie tekstury klatki", result);
    }
    result = device_->CreateRenderTargetView(frameTexture_.Get(), nullptr, &frameTarget_);
    if (FAILED(result)) {
        throw hresultError("Utworzenie celu obrotu klatki", result);
    }

    const auto vertexBytecode = compileShader(orientationVertexShader, "main", "vs_5_0");
    result = device_->CreateVertexShader(
        vertexBytecode->GetBufferPointer(),
        vertexBytecode->GetBufferSize(),
        nullptr,
        &orientationVertexShader_);
    if (FAILED(result)) {
        throw hresultError("Utworzenie shadera wierzcholkow", result);
    }

    const auto pixelBytecode = compileShader(orientationPixelShader, "main", "ps_5_0");
    result = device_->CreatePixelShader(
        pixelBytecode->GetBufferPointer(),
        pixelBytecode->GetBufferSize(),
        nullptr,
        &orientationPixelShader_);
    if (FAILED(result)) {
        throw hresultError("Utworzenie shadera obrotu", result);
    }

    D3D11_BUFFER_DESC constantDescription{};
    constantDescription.ByteWidth = sizeof(OrientationConstants);
    constantDescription.Usage = D3D11_USAGE_DEFAULT;
    constantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    result = device_->CreateBuffer(&constantDescription, nullptr, &orientationConstants_);
    if (FAILED(result)) {
        throw hresultError("Utworzenie ustawien obrotu", result);
    }
}

bool DesktopDuplicator::acquireLatestFrame(const std::uint32_t timeoutMilliseconds) {
    DXGI_OUTDUPL_FRAME_INFO frameInformation{};
    Microsoft::WRL::ComPtr<IDXGIResource> desktopResource;

    const HRESULT result = duplication_->AcquireNextFrame(
        timeoutMilliseconds, &frameInformation, &desktopResource);
    if (result == DXGI_ERROR_WAIT_TIMEOUT) {
        return false;
    }
    if (FAILED(result)) {
        throw hresultError("Pobranie klatki pulpitu", result);
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> desktopTexture;
    const HRESULT textureResult = desktopResource.As(&desktopTexture);
    if (SUCCEEDED(textureResult)) {
        context_->CopyResource(sourceTexture_.Get(), desktopTexture.Get());
        orientFrame();
    }

    const HRESULT releaseResult = duplication_->ReleaseFrame();
    if (FAILED(textureResult)) {
        throw hresultError("Pobranie tekstury pulpitu", textureResult);
    }
    if (FAILED(releaseResult)) {
        throw hresultError("Zwolnienie klatki pulpitu", releaseResult);
    }
    return true;
}

void DesktopDuplicator::orientFrame() {
    const OrientationConstants constants{
        .sourceWidth = sourceWidth_,
        .sourceHeight = sourceHeight_,
        .rotation = shaderRotation(rotation_),
    };
    context_->UpdateSubresource(orientationConstants_.Get(), 0, nullptr, &constants, 0, 0);

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width_);
    viewport.Height = static_cast<float>(height_);
    viewport.MinDepth = 0.0F;
    viewport.MaxDepth = 1.0F;

    ID3D11RenderTargetView* target = frameTarget_.Get();
    ID3D11ShaderResourceView* source = sourceView_.Get();
    ID3D11Buffer* constantsBuffer = orientationConstants_.Get();

    context_->OMSetRenderTargets(1, &target, nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(orientationVertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(orientationPixelShader_.Get(), nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, &constantsBuffer);
    context_->PSSetShaderResources(0, 1, &source);
    context_->Draw(3, 0);

    ID3D11ShaderResourceView* noSource = nullptr;
    ID3D11RenderTargetView* noTarget = nullptr;
    context_->PSSetShaderResources(0, 1, &noSource);
    context_->OMSetRenderTargets(1, &noTarget, nullptr);
}

ID3D11Device* DesktopDuplicator::device() const noexcept {
    return device_.Get();
}

ID3D11Texture2D* DesktopDuplicator::frameTexture() const noexcept {
    return frameTexture_.Get();
}

std::uint32_t DesktopDuplicator::width() const noexcept {
    return width_;
}

std::uint32_t DesktopDuplicator::height() const noexcept {
    return height_;
}

std::uint32_t DesktopDuplicator::adapterVendorId() const noexcept {
    return adapterVendorId_;
}

const std::wstring& DesktopDuplicator::adapterName() const noexcept {
    return adapterName_;
}

} // namespace nexplay::capture
