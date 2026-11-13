#pragma once

#include <d3d11.h>
#include <array>
#include <algorithm>
#include "GlassMaterial.h"
#include "Highlight.h"
#include "NebulaRender.h"
#include "RenderRecords.h"

namespace shader_tests {
using Microsoft::WRL::ComPtr;

inline void Check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) throw std::runtime_error(operation);
}
inline void Assert(bool value, const char* operation) {
    if (!value) throw std::runtime_error(operation);
}

struct Target {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
};
struct Constants {
    std::array<float, 4> output{}, blur{};
    std::array<std::array<float, 4>, 8> bounds{}, material{};
    std::array<float, 4> display{1, 1, 22, 3};
};
static_assert(sizeof(Constants) == 304);

inline void Run() {
    // This separate hardware device belongs solely to this shader test; the DLL's
    // immediate context remains exclusively on its own render thread.
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL obtained;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &device, &obtained, &context), "Create shader-test hardware device");
    auto target = [&](UINT width, UINT height, const void* pixels = nullptr) {
        Target result;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{pixels, width * 4, 0};
        Check(device->CreateTexture2D(&desc, pixels ? &data : nullptr, &result.texture), "Create shader-test texture");
        Check(device->CreateRenderTargetView(result.texture.Get(), nullptr, &result.rtv), "Create shader-test RTV");
        Check(device->CreateShaderResourceView(result.texture.Get(), nullptr, &result.srv), "Create shader-test SRV");
        return result;
    };
    auto compile = [&](const char* entry, const char* profile) {
        ComPtr<ID3DBlob> blob, error;
        Check(D3DCompile(GlassShader, strlen(GlassShader), "Glass pixel test", nullptr, nullptr,
            entry, profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &error), "Compile glass pixel-test shader");
        return blob;
    };
    ComPtr<ID3D11VertexShader> vertex;
    auto vs = compile("FullVS", "vs_5_0");
    Check(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex), "Create pixel-test VS");
    std::array<ComPtr<ID3D11PixelShader>, 4> shaders;
    const char* entries[]{"DownsamplePS", "BlurPS", "CompositePS"};
    for (int i = 0; i < 3; ++i) {
        auto blob = compile(entries[i], "ps_5_0");
        Check(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shaders[i]),
            "Create pixel-test PS");
    }
    constexpr const char* previousDownsample = R"(
cbuffer Glass : register(b0) { float4 outputInfo; float4 blurInfo; };
Texture2D<float4> original : register(t0);
SamplerState linearSampler : register(s0);
struct Fullscreen { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
float4 PS(Fullscreen input) : SV_TARGET {
    float2 texel = blurInfo.xy;
    return 0.25 * (
        original.SampleLevel(linearSampler, input.uv + texel * float2(-1,-1), 0) +
        original.SampleLevel(linearSampler, input.uv + texel * float2( 1,-1), 0) +
        original.SampleLevel(linearSampler, input.uv + texel * float2(-1, 1), 0) +
        original.SampleLevel(linearSampler, input.uv + texel * float2( 1, 1), 0));
})";
    ComPtr<ID3DBlob> previousCode;
    Check(D3DCompile(previousDownsample, strlen(previousDownsample), "Previous quarter downsample", nullptr, nullptr,
        "PS", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &previousCode, nullptr), "Compile previous downsample comparison");
    Check(device->CreatePixelShader(previousCode->GetBufferPointer(), previousCode->GetBufferSize(), nullptr, &shaders[3]),
        "Create previous downsample comparison");
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(Constants); buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> constants;
    Check(device->CreateBuffer(&buffer, nullptr, &constants), "Create pixel-test constants");
    D3D11_SAMPLER_DESC sample{};
    sample.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sample.AddressU = sample.AddressV = sample.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sample.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;
    Check(device->CreateSamplerState(&sample, &sampler), "Create pixel-test sampler");
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> rasterizer;
    Check(device->CreateRasterizerState(&raster, &rasterizer), "Create pixel-test rasterizer");
    context->RSSetState(rasterizer.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
    context->PSSetSamplers(0, 1, sampler.GetAddressOf());
    constexpr UINT width = 128, height = 96;
    std::vector<unsigned char> checker(width * height * 4);
    for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
        unsigned char value = ((x / 2 + y / 2) & 1) ? 210 : 50;
        size_t index = (static_cast<size_t>(y) * width + x) * 4;
        checker[index] = checker[index + 1] = checker[index + 2] = value;
        checker[index + 3] = 255;
    }
    auto source = target(width, height, checker.data());
    auto first = target(width / 8, height / 8), second = target(width / 8, height / 8);
    auto final = target(width, height);
    auto pass = [&](Target& destination, UINT w, UINT h, int shader, Target& input,
                    ID3D11ShaderResourceView* other, const Constants& data) {
        ID3D11ShaderResourceView* empty[]{nullptr, nullptr};
        context->PSSetShaderResources(0, 2, empty);
        context->OMSetRenderTargets(1, destination.rtv.GetAddressOf(), nullptr);
        context->UpdateSubresource(constants.Get(), 0, nullptr, &data, 0, 0);
        D3D11_VIEWPORT viewport{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->PSSetShader(shaders[shader].Get(), nullptr, 0);
        ID3D11ShaderResourceView* views[]{input.srv.Get(), other};
        context->PSSetShaderResources(0, 2, views);
        context->Draw(3, 0);
    };
    Constants data{};
    data.output = {width, height, 2, 0};
    data.bounds[0] = {24, 16, 52, 60};
    data.material[0] = {12, 0, 1, 0};
    data.bounds[1] = {92, 28, 30, 40};
    data.material[1] = {8, 0, 1, 0};
    data.blur = {1.0f / width, 1.0f / height, 0, 0};
    pass(first, width / 8, height / 8, 0, source, nullptr, data);
    for (int iteration = 0; iteration < nebula_glass::Iterations; ++iteration) {
        data.blur = {8.0f / width, 8.0f / height, nebula_glass::KernelStepPerScale, 0};
        pass(second, width / 8, height / 8, 1, first, nullptr, data);
        data.blur[2] = 0; data.blur[3] = nebula_glass::KernelStepPerScale;
        pass(first, width / 8, height / 8, 1, second, nullptr, data);
    }
    pass(final, width, height, 2, source, first.srv.Get(), data);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    D3D11_TEXTURE2D_DESC stagingDesc{};
    final.texture->GetDesc(&stagingDesc);
    stagingDesc.Usage = D3D11_USAGE_STAGING; stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    Check(device->CreateTexture2D(&stagingDesc, nullptr, &staging), "Create glass pixel readback");
    context->CopyResource(staging.Get(), final.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read glass test pixels");
    auto pixel = [&](UINT x, UINT y) {
        return static_cast<unsigned char*>(mapped.pData)[static_cast<size_t>(y) * mapped.RowPitch + x * 4];
    };
    int outsideChanged = 0, insideChanged = 0, cornerChanged = 0;
    for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
        int difference = std::abs(static_cast<int>(pixel(x, y)) - checker[(static_cast<size_t>(y) * width + x) * 4]);
        if (x < 2 && y < 2 && difference > 2) ++outsideChanged;
        if (x > 38 && x < 60 && y > 32 && y < 60 && difference > 25) ++insideChanged;
        if (x == 24 && y == 16 && pixel(x, y) > checker[(static_cast<size_t>(y) * width + x) * 4] + 2) ++cornerChanged;
    }
    bool secondPanelBlurred = std::abs(static_cast<int>(pixel(106, 44)) - checker[(44 * width + 106) * 4]) > 25;
    int interiorVariation = std::abs(static_cast<int>(pixel(48, 40)) - pixel(50, 40));
    context->Unmap(staging.Get(), 0);
    Assert(outsideChanged == 0, "Glass changed pixels outside its panels and bounded shadows");
    Assert(insideChanged > 450 && interiorVariation < 8, "Glass tint substituted for real backdrop blur");
    Assert(cornerChanged == 0, "Glass rounded corners must contain unblurred background with only exterior shadow");
    Assert(secondPanelBlurred, "Second glass rectangle was not composited");
    std::cout << "PASS glass GPU pixel readback: real Gaussian blur, bounded exterior shadows, rounded corners, two panels\n";

    auto glassPixels = [&](float opacity, float secondOpacity) {
        data.material[0] = {12, 0.55f, opacity, 0};
        data.material[1] = {8, 0.68f, secondOpacity, 0};
        pass(final, width, height, 2, source, first.srv.Get(), data);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->CopyResource(staging.Get(), final.texture.Get());
        D3D11_MAPPED_SUBRESOURCE pixels{};
        Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &pixels), "Read glass opacity pixels");
        std::vector<unsigned char> copy(width * height * 4);
        for (UINT row = 0; row < height; ++row)
            memcpy(copy.data() + row * width * 4, static_cast<unsigned char*>(pixels.pData) + row * pixels.RowPitch, width * 4);
        context->Unmap(staging.Get(), 0);
        return copy;
    };
    auto zeroOpacity = glassPixels(0, 0);
    auto halfOpacity = glassPixels(0.5f, 0);
    auto fullOpacity = glassPixels(1, 0);
    Assert(zeroOpacity == checker, "Opacity zero must exactly preserve every unmodified scene pixel");
    auto linear = [](unsigned char byte) {
        double c = byte / 255.0;
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    auto encoded = [](double c) {
        return std::lround(255 * (c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1 / 2.4) - 0.055));
    };
    int changed = 0;
    for (size_t i = 0; i < checker.size(); ++i) {
        if ((i & 3) == 3) continue;
        auto expected = encoded((linear(zeroOpacity[i]) + linear(fullOpacity[i])) * 0.5);
        Assert(std::abs(static_cast<long>(halfOpacity[i]) - expected) <= 2,
            "Opacity 0.5 must fade the complete glass material, including blur, tint, grain and rim");
        changed += fullOpacity[i] != zeroOpacity[i];
    }
    Assert(changed > 5000, "Full glass opacity must have a visible material effect");
    auto independent = glassPixels(1, 0);
    for (UINT y = 28; y < 68; ++y) for (UINT x = 102; x < 122; ++x)
        for (UINT channel = 0; channel < 4; ++channel) {
            size_t index = (static_cast<size_t>(y) * width + x) * 4 + channel;
            Assert(independent[index] == checker[index], "Per-panel opacity zero was lost in a mixed atomic batch");
        }
    std::cout << "PASS glass opacity GPU readback: zero pixel identity, half linear blend, full material,\n"
              << "     rim/tint/grain fade together and mixed per-panel opacity is independent\n";

    auto readTarget = [&](Target& input, UINT w, UINT h) {
        context->OMSetRenderTargets(0, nullptr, nullptr);
        D3D11_TEXTURE2D_DESC desc{};
        input.texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> copy;
        Check(device->CreateTexture2D(&desc, nullptr, &copy), "Create material-quality readback");
        context->CopyResource(copy.Get(), input.texture.Get());
        D3D11_MAPPED_SUBRESOURCE pixels{};
        Check(context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &pixels), "Read material-quality pixels");
        std::vector<unsigned char> result(static_cast<size_t>(w) * h * 4);
        for (UINT row = 0; row < h; ++row)
            memcpy(result.data() + row * w * 4, static_cast<unsigned char*>(pixels.pData) + row * pixels.RowPitch, w * 4);
        context->Unmap(copy.Get(), 0);
        return result;
    };
    auto measuredRadius = [&](float scale, UINT divisor, int iterations, float kernel) {
        constexpr UINT w = 256, h = 64;
        std::vector<unsigned char> edge(w * h * 4, 255);
        for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w / 2; ++x)
            edge[(y * w + x) * 4] = edge[(y * w + x) * 4 + 1] = edge[(y * w + x) * 4 + 2] = 0;
        auto input = target(w, h, edge.data());
        auto ping = target(w / divisor, h / divisor), pong = target(w / divisor, h / divisor);
        auto output = target(w, h);
        Constants settings{};
        settings.output = {w, h, 0, 0};
        settings.blur = {1.0f / w, 1.0f / h, 0, 0};
        pass(ping, w / divisor, h / divisor, divisor == 4 ? 3 : 0, input, nullptr, settings);
        for (int i = 0; i < iterations; ++i) {
            settings.blur = {static_cast<float>(divisor) / w, static_cast<float>(divisor) / h, kernel * scale, 0};
            pass(pong, w / divisor, h / divisor, 1, ping, nullptr, settings);
            settings.blur[2] = 0; settings.blur[3] = kernel * scale;
            pass(ping, w / divisor, h / divisor, 1, pong, nullptr, settings);
        }
        pass(output, w, h, 2, ping, nullptr, settings);
        auto pixels = readTarget(output, w, h);
        int low = -1, high = -1;
        for (UINT x = 0; x < w; ++x) {
            double value = linear(pixels[((h / 2) * w + x) * 4]);
            if (low < 0 && value >= 0.1) low = static_cast<int>(x);
            if (high < 0 && value >= 0.9) high = static_cast<int>(x);
        }
        Assert(low >= 0 && high > low, "Blur edge response could not be measured");
        return (high - low) * 3.0 / (2.563103 * scale);
    };
    double previousRadius = measuredRadius(1, 4, 1, 1);
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        double radius = measuredRadius(scale, nebula_glass::Downsample, nebula_glass::Iterations, nebula_glass::KernelStepPerScale);
        std::cout << "Glass measured three-sigma radius: " << radius << " DIPs at scale " << scale
                  << " (old quarter-resolution pair: " << previousRadius << " DIPs)\n";
        Assert(radius >= 28 && radius <= 36, "Glass diffusion radius must stay approximately 32 DIPs across DPI");
        if (scale == 1) Assert(radius > previousRadius * 1.25, "Glass blur is not materially wider than the previous pass");
    }

    std::vector<unsigned char> uniformPixels(width * height * 4, 160);
    for (size_t i = 3; i < uniformPixels.size(); i += 4) uniformPixels[i] = 255;
    auto uniform = target(width, height, uniformPixels.data());
    auto materialImage = [&](Target& backdrop, bool light, float shadowRadius) {
        Constants settings{};
        settings.output = {width, height, 1, light ? 1.0f : 0.0f};
        settings.bounds[0] = {24, 16, 52, 60};
        settings.material[0] = {12, light ? 0.68f : 0.55f, 1, 0};
        settings.display = {1, 1, shadowRadius, 3};
        pass(final, width, height, 2, backdrop, backdrop.srv.Get(), settings);
        return readTarget(final, width, height);
    };
    auto at = [&](const std::vector<unsigned char>& image, UINT x, UINT y, UINT channel = 0) {
        return static_cast<int>(image[(y * width + x) * 4 + channel]);
    };
    for (bool light : {false, true}) {
        auto shaded = materialImage(uniform, light, 22);
        auto withoutShadow = materialImage(uniform, light, 0);
        Assert(at(shaded, 50, 16) > at(shaded, 50, 75) + 6, "Glass top hairline must catch more light than the bottom");
        Assert(at(shaded, 50, 16) > at(shaded, 50, 17) + 8, "Top highlight is not a distinct physical-pixel hairline");
        Assert(std::abs(at(shaded, 50, 17) - at(shaded, 50, 18)) <= 2, "Glass highlight extends beyond its one-pixel width");
        Assert(at(shaded, 50, 85) < 160 && at(shaded, 0, 0) == 160, "Shadow must be soft, exterior and spatially bounded");
        for (UINT y = 28; y < 64; ++y) for (UINT x = 40; x < 60; ++x)
            Assert(at(shaded, x, y) == at(withoutShadow, x, y), "Panel shadow leaked into the material interior");
        if (light) Assert(at(shaded, 50, 15) < at(shaded, 50, 14), "Light material lacks its fine outer black hairline");
    }
    for (size_t i = 0; i < uniformPixels.size(); i += 4) {
        uniformPixels[i] = 80; uniformPixels[i + 1] = 110; uniformPixels[i + 2] = 160;
    }
    auto colorful = target(width, height, uniformPixels.data());
    auto vibrant = materialImage(colorful, false, 22);
    double actualChroma = linear(static_cast<unsigned char>(at(vibrant, 50, 45, 2))) -
                          linear(static_cast<unsigned char>(at(vibrant, 50, 45)));
    double oldChroma = (linear(160) - linear(80)) * 0.78 * 0.4;
    Assert(actualChroma > oldChroma * 1.5, "Glass vibrancy did not increase chroma relative to the former desaturation");
    std::cout << "PASS glass material: DPI-aware wide diffusion, one-pixel top-to-bottom rim,\n"
              << "     exterior-only soft shadow, increased chroma, and opacity-zero identity\n";

    ComPtr<ID3DBlob> photoVS, photoPS, errors;
    Check(D3DCompile(PhotoShader, strlen(PhotoShader), "Photo pixel test", nullptr, nullptr,
        "VS", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &photoVS, &errors), "Compile photo pixel-test VS");
    errors.Reset();
    Check(D3DCompile(PhotoShader, strlen(PhotoShader), "Photo pixel test", nullptr, nullptr,
        "PS", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &photoPS, &errors), "Compile photo pixel-test PS");
    ComPtr<ID3D11VertexShader> photoVertex;
    ComPtr<ID3D11PixelShader> photoPixel;
    Check(device->CreateVertexShader(photoVS->GetBufferPointer(), photoVS->GetBufferSize(), nullptr, &photoVertex), "Create photo test VS");
    Check(device->CreatePixelShader(photoPS->GetBufferPointer(), photoPS->GetBufferSize(), nullptr, &photoPixel), "Create photo test PS");
    nebula_records::Tile instance{{0,0,1}, 0.7f, 0xff808080, 0.65f, 0.7f / 0.65f, 1,
        {0,0,0,1}, {0,2u << 16,2u << 16,nebula_records::Flags(4,4,0,false,false)}, {1,128,0,1}};
    std::array<float, 36> camera{};
    camera[0] = camera[5] = camera[10] = camera[15] = 1;
    camera[16] = camera[21] = camera[27] = 1;
    camera[30] = 0.5f;
    camera[32] = width; camera[33] = height;
    D3D11_BUFFER_DESC photoBuffer{};
    photoBuffer.ByteWidth = sizeof(camera);
    photoBuffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA cameraData{camera.data(), 0, 0};
    ComPtr<ID3D11Buffer> cameraBuffer, instanceBuffer;
    Check(device->CreateBuffer(&photoBuffer, &cameraData, &cameraBuffer), "Create photo test camera");
    std::array<float, 8> lens{};
    photoBuffer.ByteWidth = sizeof(lens);
    ComPtr<ID3D11Buffer> lensBuffer;
    Check(device->CreateBuffer(&photoBuffer, nullptr, &lensBuffer), "Create photo test lens");
    photoBuffer.ByteWidth = sizeof(instance); photoBuffer.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    photoBuffer.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    photoBuffer.StructureByteStride = sizeof(instance);
    Check(device->CreateBuffer(&photoBuffer, nullptr, &instanceBuffer), "Create photo test instance");
    ComPtr<ID3D11ShaderResourceView> instanceView, orderView;
    Check(device->CreateShaderResourceView(instanceBuffer.Get(), nullptr, &instanceView), "Create compact photo view");
    uint32_t orderIndex = 0;
    D3D11_SUBRESOURCE_DATA orderData{&orderIndex, 0, 0};
    ComPtr<ID3D11Buffer> orderBuffer;
    photoBuffer.ByteWidth = photoBuffer.StructureByteStride = 4;
    Check(device->CreateBuffer(&photoBuffer, &orderData, &orderBuffer), "Create photo order buffer");
    Check(device->CreateShaderResourceView(orderBuffer.Get(), nullptr, &orderView), "Create photo order view");
    D3D11_SHADER_RESOURCE_VIEW_DESC arrayView{};
    arrayView.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    arrayView.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    arrayView.Texture2DArray.ArraySize = 1; arrayView.Texture2DArray.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> array;
    Check(device->CreateShaderResourceView(source.texture.Get(), &arrayView, &array), "Create photo test array");
    std::array<unsigned char, 1364> averaged{};
    size_t averageOffset = 0;
    for (UINT edge = 16; edge >= 1; edge /= 2) {
        for (UINT row = 0; row < edge; ++row) for (UINT col = 0; col < edge; ++col) {
            auto* cell = averaged.data() + averageOffset + (row * edge + col) * 4;
            cell[0] = cell[1] = cell[2] = edge == 1 ? 157 : static_cast<unsigned char>(50 + col * 180 / (edge - 1));
            cell[3] = 255;
        }
        averageOffset += edge * edge * 4;
    }
    D3D11_TEXTURE2D_DESC mosaicDesc{};
    mosaicDesc.Width = mosaicDesc.Height = 16; mosaicDesc.ArraySize = 1; mosaicDesc.MipLevels = 5;
    mosaicDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    mosaicDesc.SampleDesc.Count = 1; mosaicDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::array<D3D11_SUBRESOURCE_DATA, 5> mipData{};
    size_t mipOffset = 0;
    for (UINT mip = 0, edge = 16; mip < 5; ++mip, edge /= 2) {
        mipData[mip] = {averaged.data() + mipOffset, edge * 4, 0};
        mipOffset += edge * edge * 4;
    }
    ComPtr<ID3D11Texture2D> mosaicTexture;
    ComPtr<ID3D11ShaderResourceView> mosaicArray;
    Check(device->CreateTexture2D(&mosaicDesc, mipData.data(), &mosaicTexture), "Create averaged-mosaic test texture");
    arrayView.Texture2DArray.MipLevels = 5;
    Check(device->CreateShaderResourceView(mosaicTexture.Get(), &arrayView, &mosaicArray), "Create averaged-mosaic array view");
    std::array<unsigned char, 512> artPixels{};
    for (UINT row = 0; row < 8; ++row) for (UINT col = 0; col < 16; ++col) {
        auto* cell = artPixels.data() + (row * 16 + col) * 4;
        cell[0] = static_cast<unsigned char>(50 + col * 12);
        cell[1] = 100;
        cell[2] = static_cast<unsigned char>(50 + row * 24);
        cell[3] = 255;
    }
    D3D11_SUBRESOURCE_DATA artData{artPixels.data(), 16 * 4, 0};
    mosaicDesc.Height = 8; mosaicDesc.MipLevels = 1;
    ComPtr<ID3D11Texture2D> artTexture;
    ComPtr<ID3D11ShaderResourceView> artArray;
    Check(device->CreateTexture2D(&mosaicDesc, &artData, &artTexture), "Create 128-cell art test texture");
    arrayView.Texture2DArray.MipLevels = 1;
    Check(device->CreateShaderResourceView(artTexture.Get(), &arrayView, &artArray), "Create 128-cell art test view");
    std::array<unsigned char, 1876> hierarchy{};
    std::copy(averaged.begin(), averaged.end(), hierarchy.begin());
    std::copy(artPixels.begin(), artPixels.end(), hierarchy.begin() + averaged.size());
    D3D11_BUFFER_DESC hierarchyDesc{};
    hierarchyDesc.ByteWidth = sizeof(hierarchy);
    hierarchyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hierarchyDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    D3D11_SUBRESOURCE_DATA hierarchyData{hierarchy.data(), 0, 0};
    ComPtr<ID3D11Buffer> hierarchyBuffer;
    ComPtr<ID3D11ShaderResourceView> hierarchyView;
    Check(device->CreateBuffer(&hierarchyDesc, &hierarchyData, &hierarchyBuffer), "Create scene-resident raw mosaic record");
    D3D11_SHADER_RESOURCE_VIEW_DESC rawView{};
    rawView.Format = DXGI_FORMAT_R32_TYPELESS;
    rawView.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    rawView.BufferEx.NumElements = sizeof(hierarchy) / 4;
    rawView.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    Check(device->CreateShaderResourceView(hierarchyBuffer.Get(), &rawView, &hierarchyView), "Create raw mosaic record view");
    ComPtr<ID3D11Buffer> artRawBuffer;
    ComPtr<ID3D11ShaderResourceView> artRawView;
    hierarchyDesc.ByteWidth = sizeof(artPixels);
    D3D11_SUBRESOURCE_DATA artRawData{artPixels.data(), 0, 0};
    Check(device->CreateBuffer(&hierarchyDesc, &artRawData, &artRawBuffer), "Create lazy art record");
    rawView.BufferEx.NumElements = sizeof(artPixels) / 4;
    Check(device->CreateShaderResourceView(artRawBuffer.Get(), &rawView, &artRawView), "Create lazy art view");
    ComPtr<ID3D11SamplerState> point;
    sample.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    Check(device->CreateSamplerState(&sample, &point), "Create photo test nearest sampler");
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> coverage;
    Check(device->CreateBlendState(&blend, &coverage), "Create photo test coverage blend");
    context->OMSetBlendState(coverage.Get(), nullptr, 0xffffffff);
    context->IASetInputLayout(nullptr);
    UINT stride = sizeof(instance), offset = 0;
    ID3D11ShaderResourceView* geometry[]{instanceView.Get(), orderView.Get()};
    context->VSSetShaderResources(20, 2, geometry);
    context->VSSetShader(photoVertex.Get(), nullptr, 0);
    context->VSSetConstantBuffers(0, 1, cameraBuffer.GetAddressOf());
    context->PSSetShader(photoPixel.Get(), nullptr, 0);
    context->PSSetSamplers(1, 1, point.GetAddressOf());
    auto photograph = [&] {
        ID3D11ShaderResourceView* empty[]{nullptr, nullptr};
        context->PSSetShaderResources(0, 2, empty);
        context->OMSetRenderTargets(1, final.rtv.GetAddressOf(), nullptr);
        ID3D11ShaderResourceView* resources[]{array.Get(), array.Get(), array.Get(), array.Get(), array.Get(), array.Get(), array.Get(),
                                            hierarchyView.Get(), hierarchyView.Get(), hierarchyView.Get(), hierarchyView.Get()};
        context->PSSetShaderResources(0, 11, resources);
        context->PSSetShaderResources(11, 1, artRawView.GetAddressOf());
        const float background[]{0.01f, 0.01f, 0.01f, 1};
        context->ClearRenderTargetView(final.rtv.Get(), background);
        context->UpdateSubresource(instanceBuffer.Get(), 0, nullptr, &instance, 0, 0);
        context->UpdateSubresource(lensBuffer.Get(), 0, nullptr, lens.data(), 0, 0);
        context->PSSetConstantBuffers(1, 1, lensBuffer.GetAddressOf());
        context->DrawInstanced(6, 1, 0, 0);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->CopyResource(staging.Get(), final.texture.Get());
        D3D11_MAPPED_SUBRESOURCE pixels{};
        Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &pixels), "Read photo shader pixels");
        std::vector<unsigned char> result(width * height * 4);
        for (UINT row = 0; row < height; ++row)
            memcpy(result.data() + row * width * 4, static_cast<unsigned char*>(pixels.pData) + row * pixels.RowPitch, width * 4);
        context->Unmap(staging.Get(), 0);
        return result;
    };
    auto resolved = photograph();
    auto value = [&](const std::vector<unsigned char>& image, int x, int y, int channel = 0) {
        return static_cast<int>(image[(static_cast<size_t>(y) * width + x) * 4 + channel]);
    };
    Assert(std::abs(value(resolved, 19, 17) - value(resolved, 0, 0)) <= 12,
        "Photo shader did not clip rounded corners");
    Assert(value(resolved, 64, 48) > value(resolved, 0, 0) + 20, "Resident photograph did not render");
    instance.effects.z = 1;
    auto highlighted = photograph();
    int inkPixels = 0, blueHighlightPixels = 0;
    for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
        inkPixels += value(highlighted, x, y) > value(resolved, x, y) + 20;
        blueHighlightPixels += value(highlighted, x, y, 0) > value(highlighted, x, y, 2) + 10;
    }
    Assert(inkPixels > 150 && blueHighlightPixels == 0, "Weighted highlight needs a visible monochrome ink ring, never blue");
    instance.effects.z = 0;
    instance.alpha = nebula_highlight::Alpha(1, 0, 0, 0, 1);
    auto recedingContext = photograph();
    Assert(value(recedingContext, 64, 48) < value(resolved, 64, 48) - 10, "Non-highlighted photo did not recede");
    instance.alpha = nebula_highlight::Alpha(1, 0, 0, 0, 0);
    Assert(photograph() == resolved, "Clearing highlight did not restore identical unhighlighted pixels");
    std::cout << "PASS highlight GPU pixels: monochrome weighted ring, context at 0.18, clear restores identity\n";
    instance.pages[3] = nebula_records::Flags(4,4,1,false,false);
    auto mosaic = photograph();
    int mosaicVariation = 0, photoVariation = 0;
    for (int x = 40; x < 82; ++x) {
        mosaicVariation += std::abs(value(mosaic, x, 45) - value(mosaic, x + 1, 45));
        photoVariation += std::abs(value(resolved, x, 45) - value(resolved, x + 1, 45));
    }
    Assert(mosaicVariation < 4 && photoVariation > 100, "Pixel art did not replace detail with averaged cells");
    for (int level = 1; level <= 5; ++level) {
        instance.pages[3] = nebula_records::Flags(4,4,level,false,false);
        auto blocks = photograph();
        int columns = 1 << (level - 1);
        std::vector<int> colors;
        for (int col = 0; col < columns; ++col) {
            int x = static_cast<int>(19.2f + (col + 0.5f) * 89.6f / columns);
            colors.push_back(value(blocks, x, 48));
        }
        std::sort(colors.begin(), colors.end());
        colors.erase(std::unique(colors.begin(), colors.end()), colors.end());
        Assert(colors.size() == static_cast<size_t>(columns), "Pixels UI level does not match the expected block columns");
        if (level == 5) {
            colors.clear();
            for (int row = 0; row < 8; ++row) {
                int y = static_cast<int>(16.8f + (row + 0.5f) * 62.4f / 8);
                colors.push_back(value(blocks, 64, y, 2));
            }
            std::sort(colors.begin(), colors.end());
            colors.erase(std::unique(colors.begin(), colors.end()), colors.end());
            Assert(colors.size() == 8, "Pixels 128 mode must contain 16x8 averaged cells");
        }
    }
    instance.pages[3] = nebula_records::Flags(4,4,0,false,false); instance.effects.x = 1;
    auto selected = photograph();
    int bluePixels = 0;
    for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x)
        bluePixels += value(selected, x, y, 0) > value(selected, x, y, 2) + 25;
    Assert(bluePixels > 150, "Selection outline is not blue");
    instance.effects.x = 0; instance.effects.y = 1;
    auto hovered = photograph();
    int coloredHoverPixels = 0;
    for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x)
        coloredHoverPixels += std::abs(value(hovered, x, y, 0) - value(hovered, x, y, 2)) > 2;
    Assert(coloredHoverPixels == 0, "Hover introduced a non-monochrome accent");
    instance.effects.y = 0;
    instance.effects.w = 0.42f;
    auto distantPhoto = photograph();
    Assert(value(distantPhoto, 64, 48) < value(resolved, 64, 48), "Distance fade (con) must reduce distant-photo contrast");
    auto linearByte = [](int pixel) {
        double c = pixel / 255.0;
        return c <= .04045 ? c / 12.92 : std::pow((c + .055) / 1.055, 2.4);
    };
    for (int channel = 0; channel < 3; ++channel) {
        double ground = linearByte(channel == 0 ? 22 : 20);
        int expected = static_cast<int>(encoded(ground + .42 * (linearByte(value(resolved,64,48,channel)) - ground)));
        Assert(std::abs(value(distantPhoto,64,48,channel)-expected) <= 2, "Far shader contrast is not exactly alpha*0.42");
    }
    lens[3] = 1; lens[0] = .5f;
    auto atmosphere = photograph();
    for (float focus : {1.0f, 1.38f, 1.4f, 2.0f, 100.0f}) {
        lens[0] = focus;
        Assert(photograph() == atmosphere, "Atmosphere contains a focus-relative haze plane");
    }
    lens = {};
    std::cout << "PASS distance contrast readback: exact .42 far coefficient and no focus-relative haze plane\n";
    std::cout << "PASS photo GPU pixel readback: rounded coverage, resolved texture, Pixels 1/4/16/64/128 blocks,\n"
              << "     blue selection, monochrome hover, and distance contrast\n";

    ComPtr<ID3DBlob> lineVsCode, linePsCode;
    Check(D3DCompile(LineShader, strlen(LineShader), "Line test", nullptr, nullptr, "LineVS", "vs_5_0", 0, 0,
        &lineVsCode, nullptr), "Compile real line VS for readback");
    Check(D3DCompile(LineShader, strlen(LineShader), "Line test", nullptr, nullptr, "LinePS", "ps_5_0", 0, 0,
        &linePsCode, nullptr), "Compile real line PS for readback");
    ComPtr<ID3D11VertexShader> lineVs;
    ComPtr<ID3D11PixelShader> linePs;
    Check(device->CreateVertexShader(lineVsCode->GetBufferPointer(), lineVsCode->GetBufferSize(), nullptr, &lineVs), "Create line VS");
    Check(device->CreatePixelShader(linePsCode->GetBufferPointer(), linePsCode->GetBufferSize(), nullptr, &linePs), "Create line PS");
    D3D11_INPUT_ELEMENT_DESC lineElements[]{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}
    };
    ComPtr<ID3D11InputLayout> lineLayout;
    Check(device->CreateInputLayout(lineElements, 2, lineVsCode->GetBufferPointer(), lineVsCode->GetBufferSize(), &lineLayout),
        "Create line layout");
    std::array<float, 8> endpoints{-0.7f, 0, 1, 0.85f * 0.55f, 0.7f, 0, 1, 0.85f * 0.55f};
    photoBuffer.ByteWidth = sizeof(endpoints);
    photoBuffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    photoBuffer.MiscFlags = 0; photoBuffer.StructureByteStride = 0;
    ComPtr<ID3D11Buffer> endpointBuffer;
    D3D11_SUBRESOURCE_DATA endpointData{endpoints.data(), 0, 0};
    Check(device->CreateBuffer(&photoBuffer, &endpointData, &endpointBuffer), "Create line endpoint buffer");
    context->IASetInputLayout(lineLayout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    stride = 16;
    context->IASetVertexBuffers(0, 1, endpointBuffer.GetAddressOf(), &stride, &offset);
    context->VSSetShader(lineVs.Get(), nullptr, 0);
    context->PSSetShader(linePs.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, cameraBuffer.GetAddressOf());
    context->OMSetRenderTargets(1, final.rtv.GetAddressOf(), nullptr);
    const float background[]{0.01f, 0.01f, 0.01f, 1};
    context->ClearRenderTargetView(final.rtv.Get(), background);
    context->Draw(2, 0);
    auto lineImage = readTarget(final, width, height);
    int linePixels = 0, maxLine = 0;
    for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
        int light = value(lineImage, x, y);
        linePixels += light > 30;
        maxLine = std::max(maxLine, light);
        Assert(std::abs(light - value(lineImage, x, y, 2)) <= 1, "Line ink is not monochrome");
    }
    int expectedLine = static_cast<int>(encoded(0.01 * (1 - 0.85 * 0.55) + 0.85 * 0.55));
    Assert(linePixels >= 80 && linePixels <= 100, "Constellation line is absent or thicker than one physical pixel");
    Assert(std::abs(maxLine - expectedLine) <= 2, "Constellation line alpha must equal 0.85 * weight * mix");
    std::cout << "PASS line GPU readback: thin monochrome segment with alpha 0.85 * weight * mix\n";

}

inline void VerifyViewPixel(const NbView& snapshot, const std::array<float, 3>& position) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL feature;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &device, &feature, &context), "Create projection readback device");
    ComPtr<ID3DBlob> vertexCode, pixelCode;
    Check(D3DCompile(LineShader, strlen(LineShader), "View readback", nullptr, nullptr, "LineVS", "vs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &vertexCode, nullptr), "Compile view-projection vertex shader");
    Check(D3DCompile(LineShader, strlen(LineShader), "View readback", nullptr, nullptr, "LinePS", "ps_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &pixelCode, nullptr), "Compile view-projection pixel shader");
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    Check(device->CreateVertexShader(vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr, &vertex), "Create projection VS");
    Check(device->CreatePixelShader(pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(), nullptr, &pixel), "Create projection PS");
    D3D11_INPUT_ELEMENT_DESC elements[]{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}
    };
    ComPtr<ID3D11InputLayout> layout;
    Check(device->CreateInputLayout(elements, 2, vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), &layout), "Create projection layout");
    std::array<float, 36> camera{};
    for (int row = 0; row < 4; ++row) for (int col = 0; col < 4; ++col)
        camera[row * 4 + col] = snapshot.viewProj[col * 4 + row];
    camera[16] = camera[21] = camera[26] = camera[31] = 1;
    camera[32] = snapshot.width; camera[33] = snapshot.height;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = sizeof(camera); desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA cameraData{camera.data(), 0, 0};
    ComPtr<ID3D11Buffer> constants, centre;
    Check(device->CreateBuffer(&desc, &cameraData, &constants), "Create current-view constants");
    std::array<float, 4> world{position[0], position[1], position[2], 1};
    desc.ByteWidth = sizeof(world); desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA worldData{world.data(), 0, 0};
    Check(device->CreateBuffer(&desc, &worldData, &centre), "Create known photo-centre vertex");
    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = static_cast<UINT>(snapshot.width);
    textureDesc.Height = static_cast<UINT>(snapshot.height);
    textureDesc.MipLevels = textureDesc.ArraySize = textureDesc.SampleDesc.Count = 1;
    textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> output, staging;
    ComPtr<ID3D11RenderTargetView> target;
    Check(device->CreateTexture2D(&textureDesc, nullptr, &output), "Create projection target");
    Check(device->CreateRenderTargetView(output.Get(), nullptr, &target), "Create projection RTV");
    textureDesc.BindFlags = 0; textureDesc.Usage = D3D11_USAGE_STAGING;
    textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Check(device->CreateTexture2D(&textureDesc, nullptr, &staging), "Create projection staging");
    const float clear[]{0, 0, 0, 1};
    context->ClearRenderTargetView(target.Get(), clear);
    context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
    context->IASetInputLayout(layout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    UINT stride = 16, offset = 0;
    context->IASetVertexBuffers(0, 1, centre.GetAddressOf(), &stride, &offset);
    context->VSSetShader(vertex.Get(), nullptr, 0);
    context->PSSetShader(pixel.Get(), nullptr, 0);
    context->VSSetConstantBuffers(0, 1, constants.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
    D3D11_VIEWPORT viewport{0, 0, snapshot.width, snapshot.height, 0, 1};
    context->RSSetViewports(1, &viewport);
    context->Draw(1, 0);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->CopyResource(staging.Get(), output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read projected photo-centre pixel");
    int foundX = -1, foundY = -1, count = 0;
    for (UINT y = 0; y < textureDesc.Height; ++y) for (UINT x = 0; x < textureDesc.Width; ++x)
        if (static_cast<unsigned char*>(mapped.pData)[static_cast<size_t>(y) * mapped.RowPitch + x * 4] > 200) {
            foundX = static_cast<int>(x); foundY = static_cast<int>(y); ++count;
        }
    context->Unmap(staging.Get(), 0);
    float clip[4]{};
    for (int row = 0; row < 4; ++row) for (int col = 0; col < 4; ++col)
        clip[row] += snapshot.viewProj[row * 4 + col] * world[col];
    float x = (clip[0] / clip[3] + 1) * snapshot.width / 2;
    float y = (1 - clip[1] / clip[3]) * snapshot.height / 2;
    Assert(count == 1 && std::abs(foundX + 0.5f - x) <= 1 && std::abs(foundY + 0.5f - y) <= 1,
        "nb_get_view column-vector projection misses the rendered photo-centre pixel by more than 1 px");
    std::cout << "PASS current nb_get_view GPU projection: row-major/column-vector photo centre within 1 physical pixel\n";
}
}
