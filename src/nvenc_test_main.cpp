#include "encoding/NvencEncoder.h"
#include "storage/ReplayStorage.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace {

using Microsoft::WRL::ComPtr;

struct DeviceResources {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> texture;
};

[[nodiscard]] DeviceResources createResources(
    const std::uint32_t width,
    const std::uint32_t height) {
    constexpr std::uint32_t nvidiaVendorId = 0x10DE;

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        throw std::runtime_error("Nie mozna utworzyc fabryki DXGI.");
    }

    ComPtr<IDXGIAdapter1> nvidiaAdapter;
    for (UINT index = 0; ; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 description{};
        if (SUCCEEDED(adapter->GetDesc1(&description)) &&
            description.VendorId == nvidiaVendorId) {
            nvidiaAdapter = adapter;
            break;
        }
    }
    if (nvidiaAdapter == nullptr) {
        throw std::runtime_error("Nie znaleziono karty NVIDIA w DXGI.");
    }

    DeviceResources resources;
    D3D_FEATURE_LEVEL selectedLevel{};
    const HRESULT deviceResult = D3D11CreateDevice(
        nvidiaAdapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &resources.device,
        &selectedLevel,
        &resources.context);
    if (FAILED(deviceResult)) {
        throw std::runtime_error("Nie mozna utworzyc urzadzenia D3D11 na karcie NVIDIA.");
    }

    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(resources.device->CreateTexture2D(&description, nullptr, &resources.texture))) {
        throw std::runtime_error("Nie mozna utworzyc testowej tekstury D3D11.");
    }
    return resources;
}
void fillTestFrame(
    std::vector<std::uint32_t>& pixels,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t frameIndex) {
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint8_t blue = static_cast<std::uint8_t>((x + frameIndex * 4U) & 0xFFU);
            const std::uint8_t green = static_cast<std::uint8_t>((y + frameIndex * 2U) & 0xFFU);
            const std::uint8_t red = static_cast<std::uint8_t>((x / 4U + y / 4U) & 0xFFU);
            pixels[static_cast<std::size_t>(y) * width + x] =
                0xFF000000U |
                (static_cast<std::uint32_t>(red) << 16U) |
                (static_cast<std::uint32_t>(green) << 8U) |
                blue;
        }
    }
}

} // namespace

int wmain() {
    constexpr std::uint32_t width = 1280;
    constexpr std::uint32_t height = 720;
    constexpr std::uint32_t framesPerSecond = 60;
    constexpr std::uint32_t frameCount = 60;

    try {
        nexplay::storage::ReplayStorage storage;
        storage.ensureExists();
        const std::filesystem::path outputPath = storage.root() / L"nvenc-synthetic-test.h264";

        DeviceResources resources = createResources(width, height);
        nexplay::encoding::NvencEncoder encoder(
            resources.device.Get(),
            resources.texture.Get(),
            {
                .width = width,
                .height = height,
                .framesPerSecond = framesPerSecond,
                .bitrate = 8'000'000,
            });

        std::ofstream output(outputPath, std::ios::binary);
        if (!output) {
            throw std::runtime_error("Nie mozna utworzyc pliku testowego.");
        }

        std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * height);
        for (std::uint32_t frame = 0; frame < frameCount; ++frame) {
            fillTestFrame(pixels, width, height, frame);
            resources.context->UpdateSubresource(
                resources.texture.Get(), 0, nullptr, pixels.data(), width * 4U, 0);
            encoder.encodeFrame(output, frame);
        }
        encoder.finish();
        output.close();

        std::wcout << L"NVENC zakodowal 60 klatek testowych.\n";
        std::wcout << L"Plik: " << outputPath.wstring() << L'\n';
        std::wcout << L"Rozmiar: " << std::filesystem::file_size(outputPath) << L" bajtow\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Blad: " << error.what() << '\n';
        return 1;
    }
}
