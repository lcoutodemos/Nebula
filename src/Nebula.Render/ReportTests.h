#pragma once
#include "MipCache.h"
#include "OriginalLoader.h"
#include "DepthEffects.h"
#include <fstream>
#include <iostream>
#include <filesystem>

namespace report_tests {
inline void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
inline void Hr(HRESULT value, const char* message) { Require(SUCCEEDED(value), message); }

inline void Bc7Quality(const nebula_cache::Payload& encoded, const std::vector<unsigned char>& input) {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context; D3D_FEATURE_LEVEL feature;
    Hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &device, &feature, &context), "Create BC7 quality device");
    std::vector<D3D11_SUBRESOURCE_DATA> levels(encoded.levels);
    for (UINT i = 0, edge = encoded.edge; i < encoded.levels; ++i, edge = std::max(1u, edge / 2))
        levels[i] = {encoded.bytes.data() + encoded.offsets[i], std::max(1u, (edge + 3) / 4) * 16, 0};
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = encoded.edge; desc.MipLevels = encoded.levels; desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_BC7_UNORM_SRGB; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture, output, readback; ComPtr<ID3D11ShaderResourceView> view;
    Hr(device->CreateTexture2D(&desc, levels.data(), &texture), "Upload complete BC7 chain");
    Hr(device->CreateShaderResourceView(texture.Get(), nullptr, &view), "Create BC7 SRV");
    desc.MipLevels = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    Hr(device->CreateTexture2D(&desc, nullptr, &output), "Create BC7 decoded target");
    ComPtr<ID3D11RenderTargetView> target;
    Hr(device->CreateRenderTargetView(output.Get(), nullptr, &target), "Create BC7 decoded RTV");
    desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Hr(device->CreateTexture2D(&desc, nullptr, &readback), "Create BC7 readback");
    const char* shader = R"(
Texture2D<float4> image:register(t0); SamplerState sampleImage:register(s0);
struct V { float4 p:SV_POSITION; float2 uv:TEXCOORD0; };
V VS(uint id:SV_VertexID) { V v; v.uv=float2((id<<1)&2,id&2); v.p=float4(v.uv*float2(2,-2)+float2(-1,1),0,1);return v; }
float4 PS(V v):SV_TARGET { return image.SampleLevel(sampleImage,v.uv,0); }
)";
    ComPtr<ID3DBlob> vsCode, psCode; ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps;
    Hr(D3DCompile(shader, strlen(shader), "BC7 quality", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vsCode, nullptr), "Compile BC7 quality VS");
    Hr(D3DCompile(shader, strlen(shader), "BC7 quality", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &psCode, nullptr), "Compile BC7 quality PS");
    Hr(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs), "Create BC7 quality VS");
    Hr(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps), "Create BC7 quality PS");
    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler; Hr(device->CreateSamplerState(&samplerDesc, &sampler), "Create BC7 sampler");
    D3D11_VIEWPORT viewport{0,0,static_cast<float>(encoded.edge),static_cast<float>(encoded.edge),0,1};
    context->RSSetViewports(1, &viewport); context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs.Get(), nullptr, 0); context->PSSetShader(ps.Get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, view.GetAddressOf()); context->PSSetSamplers(0, 1, sampler.GetAddressOf());
    context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr); context->Draw(3, 0);
    context->OMSetRenderTargets(0, nullptr, nullptr); context->CopyResource(readback.Get(), output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Hr(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read actual hardware BC7 decode");
    double error = 0;
    for (UINT y = 0; y < encoded.edge; ++y) for (UINT x = 0; x < encoded.edge; ++x) for (int c = 0; c < 3; ++c) {
        int delta = static_cast<unsigned char*>(mapped.pData)[y * mapped.RowPitch + x * 4 + c] -
                    input[(y * encoded.edge + x) * 4 + c];
        error += delta * delta;
    }
    context->Unmap(readback.Get(), 0);
    double psnr = 10 * std::log10(255.0 * 255 * encoded.edge * encoded.edge * 3 / std::max(error, 1.0));
    Require(psnr >= 40, "BC7 quality fixture failed 40 dB RGB PSNR gate");
    std::cout << "PASS hardware-decoded BC7 quality: " << psnr << " dB RGB PSNR, full trilinear mip chain\n";
}

inline void Cache(const std::filesystem::path& output) {
    auto root = output / (L"cache-contract-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    auto content = root / L"content.bin";
    { std::ofstream file(content, std::ios::binary); file << "content-version-a"; }
    auto key = nebula_cache::SourceKey(content.wstring(), 7);
    std::vector<unsigned char> pixels(64 * 64 * 4);
    for (unsigned y = 0; y < 64; ++y) for (unsigned x = 0; x < 64; ++x) {
        auto* p = pixels.data() + (y * 64 + x) * 4;
        p[0] = static_cast<unsigned char>((x + y) * 2);
        p[1] = static_cast<unsigned char>(y * 4); p[2] = static_cast<unsigned char>(x * 4); p[3] = 255;
    }
    auto encoded = nebula_cache::Encode(pixels, 64, 1);
    Bc7Quality(encoded, pixels);
    Require(encoded.levels == 7 && encoded.bytes.size() == 5488, "BC7 must include the complete padded mip chain");
    {
        nebula_cache::Store cache(0xabc, root);
        nebula_cache::Payload read;
        Require(cache.Read(key, 64, read) == S_FALSE, "Missing cache record must be explicit");
        Hr(cache.Write(key, encoded), "Write versioned BC7 mip pack");
        Hr(cache.Read(key, 64, read), "Read mapped BC7 mip pack");
        Require(read.bytes == encoded.bytes && read.offsets == encoded.offsets, "Mapped pack did not preserve BC7 mip bytes");
    }
    auto pack = root / L"0000000000000abc.pack";
    auto journal = root / L"0000000000000abc.journal";
    auto manifest = root / L"0000000000000abc.manifest";
    Require(std::filesystem::exists(manifest), "Cache generation must publish an atomic manifest");
    {
        std::fstream file(pack, std::ios::binary | std::ios::in | std::ios::out);
        file.seekg(sizeof(nebula_cache::Header));
        char byte = 0; file.read(&byte, 1); byte ^= 0x40;
        file.seekp(sizeof(nebula_cache::Header)); file.write(&byte, 1);
    }
    {
        nebula_cache::Store cache(0xabc, root);
        nebula_cache::Payload read;
        Require(cache.Read(key, 64, read) == HRESULT_FROM_WIN32(ERROR_CRC), "Corrupt BC7 blocks were accepted");
        Hr(cache.Write(key, encoded), "Repair corrupt cache record using source-derived bytes");
        Hr(cache.Read(key, 64, read), "Read repaired cache record");
        Require(read.bytes == encoded.bytes, "Cache repair returned stale bytes");
    }
    { std::ofstream file(content, std::ios::binary | std::ios::trunc); file << "content-version-b"; }
    Require(nebula_cache::SourceKey(content.wstring(), 7) != key, "Content SHA-256 must invalidate same-length source edits");
    Require(nebula_cache::SourceKey(content.wstring(), 8) != nebula_cache::SourceKey(content.wstring(), 7),
            "Import/content version must participate in cache invalidation");
    {
        std::ofstream tail(journal, std::ios::binary | std::ios::app); tail.write("broken", 6);
    }
    {
        nebula_cache::Store cache(0xabc, root);
        nebula_cache::Payload read;
        Hr(cache.Read(key, 64, read), "A truncated journal must preserve its valid committed prefix");
        Require(FAILED(cache.Warning()), "Truncated journal must be reported");
    }
    std::filesystem::resize_file(pack, std::filesystem::file_size(pack) - 1);
    {
        nebula_cache::Store cache(0xabc, root);
        nebula_cache::Payload read;
        Require(cache.Read(key, 64, read) != S_OK, "Truncated pack payload was mapped past EOF or accepted");
    }
    {
        std::fstream file(pack, std::ios::binary | std::ios::in | std::ios::out);
        nebula_cache::Header header;
        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        header.version = 99;
        file.seekp(0); file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    }
    {
        nebula_cache::Store cache(0xabc, root);
        nebula_cache::Payload read;
        Require(FAILED(cache.Read(key, 64, read)), "Wrong pack version must not reach GPU upload");
    }
    std::filesystem::remove(content); std::filesystem::remove(pack);
    std::filesystem::remove(journal); std::filesystem::remove(manifest); std::filesystem::remove(root);
    std::cout << "PASS persistent BC7 cache: full mips, mapped roundtrip, CRC rejection/repair, source hashes,\n"
              << "     version validation, committed-prefix recovery and atomic manifest\n";
}

inline void WriteOriginal(const std::filesystem::path& path, int width, int height) {
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + width * height * 4;
    info.biSize = sizeof(info); info.biWidth = width; info.biHeight = -height;
    info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file), sizeof(file));
    output.write(reinterpret_cast<const char*>(&info), sizeof(info));
    std::vector<unsigned char> row(static_cast<size_t>(width) * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            auto* pixel = row.data() + x * 4;
            unsigned char value = ((x + y) & 1) ? 235 : 25;
            pixel[0] = value; pixel[1] = static_cast<unsigned char>((x * 7 + y) & 255);
            pixel[2] = value; pixel[3] = 255;
        }
        output.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
    }
    Require(output.good(), "Write bounded original sharpness fixture");
}

inline void Orientations(const std::filesystem::path& output) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    Hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "Create orientation test WIC");
    const int expected[8][6]{
        {0,1,2,3,4,5}, {2,1,0,5,4,3}, {5,4,3,2,1,0}, {3,4,5,0,1,2},
        {0,3,1,4,2,5}, {3,0,4,1,5,2}, {5,2,4,1,3,0}, {2,5,1,4,0,3}
    };
    for (USHORT orientation = 1; orientation <= 8; ++orientation) {
        auto path = output / (L"orientation-contract-" + std::to_wstring(orientation) + L".tif");
        {
            ComPtr<IWICStream> stream; ComPtr<IWICBitmapEncoder> encoder; ComPtr<IWICBitmapFrameEncode> frame;
            ComPtr<IPropertyBag2> properties;
            Hr(factory->CreateStream(&stream), "Create orientation stream");
            Hr(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "Open orientation fixture");
            Hr(factory->CreateEncoder(GUID_ContainerFormatTiff, nullptr, &encoder), "Create TIFF encoder");
            Hr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Initialize TIFF encoder");
            Hr(encoder->CreateNewFrame(&frame, &properties), "Create orientation frame");
            Hr(frame->Initialize(properties.Get()), "Initialize orientation frame");
            Hr(frame->SetSize(3, 2), "Set orientation dimensions");
            WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
            Hr(frame->SetPixelFormat(&format), "Set orientation format");
            ComPtr<IWICMetadataQueryWriter> metadata;
            Hr(frame->GetMetadataQueryWriter(&metadata), "Create orientation metadata");
            PROPVARIANT value{}; value.vt = VT_UI2; value.uiVal = orientation;
            Hr(metadata->SetMetadataByName(L"/ifd/{ushort=274}", &value), "Set EXIF orientation");
            std::array<unsigned char, 24> pixels{};
            for (int i = 0; i < 6; ++i) { pixels[i*4+2] = static_cast<unsigned char>(10 + i*40); pixels[i*4+3] = 255; }
            Hr(frame->WritePixels(2, 12, static_cast<UINT>(pixels.size()), pixels.data()), "Write orientation pixels");
            Hr(frame->Commit(), "Commit orientation frame"); Hr(encoder->Commit(), "Commit orientation file");
        }
        {
            ComPtr<IWICBitmapDecoder> decoder; ComPtr<IWICBitmapFrameDecode> frame;
            Hr(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder), "Decode orientation fixture");
            Hr(decoder->GetFrame(0, &frame), "Read orientation frame");
            auto source = nebula_original::OrientedSrgb(factory.Get(), frame.Get());
            UINT width, height; source->GetSize(&width, &height);
            auto decoded = nebula_original::Pixels(factory.Get(), source.Get(), {}, width, height, nullptr, 0);
            Require(width == (orientation <= 4 ? 3u : 2u) && height == (orientation <= 4 ? 2u : 3u), "EXIF orientation dimensions incorrect");
            for (int i = 0; i < 6; ++i) {
                if (decoded.pixels[i*4+2] != 10 + expected[orientation-1][i]*40)
                    std::cerr << "Orientation " << orientation << " pixel " << i << ": "
                              << static_cast<int>(decoded.pixels[i*4+2]) << " expected " << 10 + expected[orientation-1][i]*40 << '\n';
                Require(decoded.pixels[i*4+2] == 10 + expected[orientation-1][i]*40, "EXIF orientation pixel order incorrect");
            }
        }
        std::filesystem::remove(path);
    }
    std::cout << "PASS original orientation: all eight EXIF transforms preserve exact pixel order\n";
}
}
