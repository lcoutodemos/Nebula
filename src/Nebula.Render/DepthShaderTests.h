#pragma once
#include "OriginalLoader.h"
#include "RenderRecords.h"
#include "DepthEffects.h"

namespace depth_tests {
using Microsoft::WRL::ComPtr;
inline void Check(HRESULT hr, const char* message) { if (FAILED(hr)) throw std::runtime_error(message); }
inline void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Surface {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11ShaderResourceView> view;
};

inline void Run(const std::filesystem::path& originalPath) {
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "Create original test WIC");
    ComPtr<IWICBitmapDecoder> decoder;
    Check(factory->CreateDecoderFromFilename(originalPath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder), "Open sharp original fixture");
    ComPtr<IWICBitmapFrameDecode> frame;
    Check(decoder->GetFrame(0, &frame), "Read sharp original frame");
    auto source = nebula_original::OrientedSrgb(factory.Get(), frame.Get());
    UINT sourceWidth, sourceHeight; source->GetSize(&sourceWidth, &sourceHeight);
    nebula_original::Request request; request.id = 7; request.path = originalPath.wstring();
    auto full = nebula_original::Pixels(factory.Get(), source.Get(), request, sourceWidth, sourceHeight, nullptr, 0);
    auto low = nebula_original::Pixels(factory.Get(), source.Get(), request, sourceWidth, sourceHeight, nullptr, 224);
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context; D3D_FEATURE_LEVEL feature;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &device, &feature, &context), "Create original/DoF GPU test device");
    auto surface = [&](UINT width, UINT height, DXGI_FORMAT format) {
        Surface result; D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height; desc.Format = format;
        desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        Check(device->CreateTexture2D(&desc, nullptr, &result.texture), "Create effect test surface");
        Check(device->CreateRenderTargetView(result.texture.Get(), nullptr, &result.target), "Create effect RTV");
        Check(device->CreateShaderResourceView(result.texture.Get(), nullptr, &result.view), "Create effect SRV");
        return result;
    };
    auto read = [&](Surface& input, UINT width, UINT height, UINT bytes) {
        context->OMSetRenderTargets(0, nullptr, nullptr);
        D3D11_TEXTURE2D_DESC desc{}; input.texture->GetDesc(&desc);
        desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        Check(device->CreateTexture2D(&desc, nullptr, &staging), "Create effect readback");
        context->CopyResource(staging.Get(), input.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read effect pixels");
        std::vector<unsigned char> result(static_cast<size_t>(width) * height * bytes);
        for (UINT y = 0; y < height; ++y) memcpy(result.data() + y * width * bytes,
            static_cast<unsigned char*>(mapped.pData) + y * mapped.RowPitch, width * bytes);
        context->Unmap(staging.Get(), 0);
        return result;
    };
    constexpr UINT width = 768, height = 512;
    auto scene = surface(width, height, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    auto depth = surface(width, height, DXGI_FORMAT_R32_FLOAT);
    auto mask = surface(width, height, DXGI_FORMAT_R8_UNORM);
    auto halfA = surface(width / 2, height / 2, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    auto halfB = surface(width / 2, height / 2, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    auto final = surface(width, height, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    auto compile = [&](const char* sourceCode, const char* entry, const char* profile) {
        ComPtr<ID3DBlob> code, errors;
        HRESULT hr = D3DCompile(sourceCode, strlen(sourceCode), entry, nullptr, nullptr, entry, profile, 0, 0, &code, &errors);
        if (FAILED(hr) && errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer());
        Check(hr, "Compile original/DoF test shader");
        return code;
    };
    auto photoVsCode = compile(PhotoShader, "VS", "vs_5_0"), photoPsCode = compile(PhotoShader, "PS", "ps_5_0");
    auto opaqueCode = compile(PhotoShader, "OpaqueDepthPS", "ps_5_0");
    auto fullVsCode = compile(GlassShader, "FullVS", "vs_5_0");
    ComPtr<ID3D11VertexShader> photoVs, fullVs; ComPtr<ID3D11PixelShader> photoPs, opaquePs;
    Check(device->CreateVertexShader(photoVsCode->GetBufferPointer(), photoVsCode->GetBufferSize(), nullptr, &photoVs), "Create original VS");
    Check(device->CreateVertexShader(fullVsCode->GetBufferPointer(), fullVsCode->GetBufferSize(), nullptr, &fullVs), "Create effect VS");
    Check(device->CreatePixelShader(photoPsCode->GetBufferPointer(), photoPsCode->GetBufferSize(), nullptr, &photoPs), "Create original PS");
    Check(device->CreatePixelShader(opaqueCode->GetBufferPointer(), opaqueCode->GetBufferSize(), nullptr, &opaquePs), "Create opaque-interior PS");
    D3D11_TEXTURE2D_DESC hardwareDepthDesc{};
    hardwareDepthDesc.Width = width; hardwareDepthDesc.Height = height;
    hardwareDepthDesc.MipLevels = hardwareDepthDesc.ArraySize = hardwareDepthDesc.SampleDesc.Count = 1;
    hardwareDepthDesc.Format = DXGI_FORMAT_D32_FLOAT; hardwareDepthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> hardwareDepth;
    ComPtr<ID3D11DepthStencilView> depthView;
    ComPtr<ID3D11DepthStencilState> depthState;
    Check(device->CreateTexture2D(&hardwareDepthDesc, nullptr, &hardwareDepth), "Create overlap depth texture");
    Check(device->CreateDepthStencilView(hardwareDepth.Get(), nullptr, &depthView), "Create overlap DSV");
    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthEnable = TRUE; depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depthDesc.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    Check(device->CreateDepthStencilState(&depthDesc, &depthState), "Create reverse-Z overlap state");
    std::array<ComPtr<ID3D11PixelShader>, 3> effect;
    const char* entries[]{"DownPS","BlurPS","ResolvePS"};
    for (int i = 0; i < 3; ++i) {
        auto code = compile(DepthShader, entries[i], "ps_5_0");
        Check(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &effect[i]), "Create protected DoF PS");
    }
    std::array<float, 36> camera{};
    camera[0] = camera[5] = camera[10] = camera[15] = 1;
    camera[16] = camera[21] = camera[27] = 1; camera[30] = 0.5f;
    camera[32] = width; camera[33] = height;
    auto buffer = [&](UINT size, UINT binding, const void* data, UINT stride = 0) {
        ComPtr<ID3D11Buffer> result;
        D3D11_BUFFER_DESC desc{}; desc.ByteWidth = size; desc.BindFlags = binding;
        if (stride) { desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; desc.StructureByteStride = stride; }
        D3D11_SUBRESOURCE_DATA input{data, 0, 0};
        Check(device->CreateBuffer(&desc, data ? &input : nullptr, &result), "Create original test buffer");
        return result;
    };
    auto cameraBuffer = buffer(sizeof(camera), D3D11_BIND_CONSTANT_BUFFER, camera.data());
    auto lensBuffer = buffer(32, D3D11_BIND_CONSTANT_BUFFER, nullptr);
    auto dofBuffer = buffer(32, D3D11_BIND_CONSTANT_BUFFER, nullptr);
    nebula_records::Tile tiles[]{
        {{0,0,2},2.2f,0xff808080,2.2f,1,1,{0,0,0,0.5f},{UINT32_MAX,2u << 16 | 1,2u << 16 | 1,nebula_records::Flags(4,4,0,false,false)},{1,400,0,1}},
        {{0,0,1},static_cast<float>(sourceWidth)/width,0xff808080,static_cast<float>(sourceHeight)/height,
         static_cast<float>(sourceWidth)/sourceHeight,1,{1,0,0,1},{UINT32_MAX,2u << 16,2u << 16,nebula_records::Flags(4,4,0,false,true)},{1,640,0,1}}
    };
    auto tileBuffer = buffer(sizeof(tiles), D3D11_BIND_SHADER_RESOURCE, tiles, sizeof(nebula_records::Tile));
    uint32_t order[]{0,1};
    auto orderBuffer = buffer(sizeof(order), D3D11_BIND_SHADER_RESOURCE, order, 4);
    ComPtr<ID3D11ShaderResourceView> tileView, orderView;
    Check(device->CreateShaderResourceView(tileBuffer.Get(), nullptr, &tileView), "Create original tile view");
    Check(device->CreateShaderResourceView(orderBuffer.Get(), nullptr, &orderView), "Create original order view");
    D3D11_TEXTURE2D_DESC imageDesc{};
    imageDesc.Width = full.width; imageDesc.Height = full.height; imageDesc.ArraySize = imageDesc.MipLevels = imageDesc.SampleDesc.Count = 1;
    imageDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; imageDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA fullData{full.pixels.data(), full.width * 4, 0};
    ComPtr<ID3D11Texture2D> original, previews; ComPtr<ID3D11ShaderResourceView> originalView, previewView;
    Check(device->CreateTexture2D(&imageDesc, &fullData, &original), "Upload actual original pixels, not a thumbnail");
    Check(device->CreateShaderResourceView(original.Get(), nullptr, &originalView), "Create original view");
    std::vector<unsigned char> background(low.pixels.size());
    for (UINT y = 0; y < low.height; ++y) for (UINT x = 0; x < low.width; ++x) {
        auto* p = background.data() + (y * low.width + x) * 4;
        bool high = ((x / 2 + y / 2) & 1) != 0;
        p[0] = high ? 40 : 200; p[1] = high ? 180 : 60; p[2] = high ? 210 : 30; p[3] = 255;
    }
    imageDesc.Width = low.width; imageDesc.Height = low.height; imageDesc.ArraySize = 2;
    D3D11_SUBRESOURCE_DATA previewData[]{ {low.pixels.data(), low.width * 4, 0}, {background.data(), low.width * 4, 0} };
    Check(device->CreateTexture2D(&imageDesc, previewData, &previews), "Upload low-resolution reference");
    Check(device->CreateShaderResourceView(previews.Get(), nullptr, &previewView), "Create reference mip view");
    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler; Check(device->CreateSamplerState(&samplerDesc, &sampler), "Create original sampler");
    D3D11_BLEND_DESC blend{};
    blend.IndependentBlendEnable = TRUE;
    for (int i = 0; i < 3; ++i) blend.RenderTarget[i].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    blend.RenderTarget[0].BlendEnable = TRUE; blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    ComPtr<ID3D11BlendState> blending; Check(device->CreateBlendState(&blend, &blending), "Create independent color/depth/mask blend");
    auto render = [&](bool originalEnabled, bool light, bool haze, bool prepass = false) {
        tiles[0].pages[3] = nebula_records::Flags(4,4,0,light,false);
        tiles[1].pages[3] = nebula_records::Flags(4,4,0,light,originalEnabled);
        context->UpdateSubresource(tileBuffer.Get(), 0, nullptr, tiles, 0, 0);
        float lens[]{1, originalEnabled ? 1.0f : 0.0f, 1, haze ? 1.0f : 0.0f, 0,0,1,1};
        context->UpdateSubresource(lensBuffer.Get(), 0, nullptr, lens, 0, 0);
        ID3D11ShaderResourceView* empty[16]{};
        context->PSSetShaderResources(0, 16, empty);
        ID3D11RenderTargetView* targets[]{scene.target.Get(), depth.target.Get(), mask.target.Get()};
        context->OMSetRenderTargets(3, targets, depthView.Get());
        context->OMSetDepthStencilState(depthState.Get(), 0);
        context->ClearDepthStencilView(depthView.Get(), D3D11_CLEAR_DEPTH, 0, 0);
        const float clear[]{0,0,0,0};
        context->ClearRenderTargetView(scene.target.Get(), clear); context->ClearRenderTargetView(depth.target.Get(), clear);
        context->ClearRenderTargetView(mask.target.Get(), clear);
        context->OMSetBlendState(blending.Get(), nullptr, 0xffffffff);
        D3D11_VIEWPORT viewport{0,0,width,height,0,1}; context->RSSetViewports(1, &viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); context->IASetInputLayout(nullptr);
        context->VSSetShader(photoVs.Get(), nullptr, 0); context->PSSetShader(photoPs.Get(), nullptr, 0);
        context->VSSetConstantBuffers(0, 1, cameraBuffer.GetAddressOf());
        context->PSSetConstantBuffers(1, 1, lensBuffer.GetAddressOf());
        ID3D11ShaderResourceView* geometry[]{tileView.Get(),orderView.Get()}; context->VSSetShaderResources(20, 2, geometry);
        context->PSSetShaderResources(5, 1, previewView.GetAddressOf());
        context->PSSetShaderResources(15, 1, originalView.GetAddressOf());
        context->PSSetSamplers(0, 1, sampler.GetAddressOf());
        if (prepass) {
            context->OMSetRenderTargets(0, nullptr, depthView.Get());
            context->PSSetShader(opaquePs.Get(), nullptr, 0);
            context->DrawInstanced(6, 2, 0, 0);
            context->OMSetRenderTargets(3, targets, depthView.Get());
            context->PSSetShader(photoPs.Get(), nullptr, 0);
        }
        context->DrawInstanced(6, 2, 0, 0);
        return read(scene, width, height, 4);
    };
    auto lowImage = render(false, false, false);
    auto sharp = render(true, false, true);
    Require(render(true, false, true, true) == sharp, "Opaque interior prepass changed rounded AA overlap pixels");
    auto protectedMask = read(mask, width, height, 1);
    double difference = 0, sourceError = 0;
    for (UINT y = height / 2 - 40; y < height / 2 + 40; ++y) for (UINT x = width / 2 - 50; x < width / 2 + 50; ++x)
        for (UINT c = 0; c < 3; ++c) {
            size_t at = (y * width + x) * 4 + c;
            size_t sourceAt = ((y + sourceHeight / 2 - height / 2) * sourceWidth + x + sourceWidth / 2 - width / 2) * 4 + c;
            difference += std::abs(int(sharp[at]) - lowImage[at]);
            sourceError += std::abs(int(sharp[at]) - full.pixels[sourceAt]);
        }
    Require(difference / (100 * 80 * 3) > 20, "Focused original must restore detail absent from the 224 reference");
    Require(sourceError / (100 * 80 * 3) < 0.01, "Focused 1:1 interior must preserve original pixels");
    auto effectPass = [&](Surface& destination, UINT w, UINT h, int shader, ID3D11ShaderResourceView* input,
                          ID3D11ShaderResourceView* blur, float x, float y) {
        ID3D11ShaderResourceView* empty[4]{}; context->PSSetShaderResources(0, 4, empty);
        context->OMSetRenderTargets(1, destination.target.GetAddressOf(), nullptr);
        float lens[]{1,18,6,0,1.0f/width,1.0f/height,x,y};
        context->UpdateSubresource(dofBuffer.Get(), 0, nullptr, lens, 0, 0);
        context->PSSetConstantBuffers(0, 1, dofBuffer.GetAddressOf());
        D3D11_VIEWPORT viewport{0,0,static_cast<float>(w),static_cast<float>(h),0,1}; context->RSSetViewports(1, &viewport);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context->VSSetShader(fullVs.Get(), nullptr, 0); context->PSSetShader(effect[shader].Get(), nullptr, 0);
        ID3D11ShaderResourceView* views[]{input,depth.view.Get(),blur,mask.view.Get()};
        context->PSSetShaderResources(0, 4, views); context->Draw(3, 0);
    };
    for (bool light : {false, true}) {
        auto withoutHaze = render(true, light, false);
        sharp = render(true, light, true);
        protectedMask = read(mask, width, height, 1);
        double beforeChroma = 0, afterChroma = 0;
        for (size_t p = 0; p < protectedMask.size(); ++p) {
            UINT x = static_cast<UINT>(p % width), y = static_cast<UINT>(p / width);
            if (protectedMask[p] >= 26 && x > 96 && x < width - 96 && y > 96 && y < height - 96)
                Require(memcmp(withoutHaze.data() + p * 4, sharp.data() + p * 4, 4) == 0, "Atmosphere changed protected photographic pixels");
            else if (protectedMask[p] < 26) {
                beforeChroma += std::max({withoutHaze[p*4],withoutHaze[p*4+1],withoutHaze[p*4+2]}) -
                                std::min({withoutHaze[p*4],withoutHaze[p*4+1],withoutHaze[p*4+2]});
                afterChroma += std::max({sharp[p*4],sharp[p*4+1],sharp[p*4+2]}) -
                               std::min({sharp[p*4],sharp[p*4+1],sharp[p*4+2]});
            }
        }
        Require(afterChroma < beforeChroma, "Optional atmosphere must subtly reduce chroma along the contrast curve");
        effectPass(halfA, width/2, height/2, 0, scene.view.Get(), nullptr, 0, 0);
        effectPass(halfB, width/2, height/2, 1, halfA.view.Get(), nullptr, 1, 0);
        effectPass(halfA, width/2, height/2, 1, halfB.view.Get(), nullptr, 0, 1);
        effectPass(final, width, height, 2, scene.view.Get(), halfA.view.Get(), 0, 0);
        auto after = read(final, width, height, 4);
        size_t unchanged = 0, changedBackground = 0;
        for (size_t p = 0; p < protectedMask.size(); ++p) {
            if (protectedMask[p] >= 26) {
                Require(memcmp(sharp.data() + p * 4, after.data() + p * 4, 4) == 0, "DoF changed a protected focused tile pixel");
                ++unchanged;
            } else if (std::abs(int(sharp[p*4]) - after[p*4]) > 2) ++changedBackground;
        }
        Require(unchanged > 200000 && changedBackground > 500, "Depth test did not exercise both sharp focus and blurred background");
    }
    std::cout << "PASS focused-original GPU sharpness: 1:1 source pixels, real refinement over 224;\n"
              << "     dark/light atmosphere recedes background, DoF leaves all protected pixels byte-identical\n";
}
}
