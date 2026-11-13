#pragma once
#include "OriginalLoader.h"
#include <cstring>

namespace nebula_decode {
struct Thumbnail {
    float aspect = 1;
    std::array<float, 4> uv{};
    UINT x = 0, y = 0, width = 0, height = 0, sourceWidth = 0, sourceHeight = 0;
    nebula_original::MetadataWarning metadata;
    std::vector<unsigned char> pixels;
};

inline Thumbnail Decode(IWICImagingFactory* factory, const std::wstring& path, UINT edge, bool original) {
    using nebula_original::Check;
    using nebula_original::Failure;
    if (!edge || edge > 512) throw Failure{E_INVALIDARG, L"Validate thumbnail edge"};
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Check(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder),
          original ? L"Open proximity original" : L"Open normalized thumbnail");
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    Check(decoder->GetFrame(0, &frame), L"Read photograph frame");
    UINT width, height;
    Check(frame->GetSize(&width, &height), L"Read photograph dimensions");
    if (!width || !height || (!original && (width > 1024 || height > 1024)))
        throw Failure{HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE), L"Validate thumbnail dimensions"};
    nebula_original::Request request;
    auto image = nebula_original::FramePixels(factory, frame.Get(), request, nullptr, edge, true);
    Thumbnail result;
    result.aspect = static_cast<float>(image.sourceWidth) / image.sourceHeight;
    result.sourceWidth = image.sourceWidth; result.sourceHeight = image.sourceHeight;
    result.width = image.width; result.height = image.height;
    result.x = (edge - image.width) / 2; result.y = (edge - image.height) / 2;
    result.metadata = image.metadata;
    result.pixels.resize(static_cast<size_t>(edge) * edge * 4);
    for (UINT row = 0; row < image.height; ++row)
        memcpy(result.pixels.data() + ((row + result.y) * edge + result.x) * 4,
               image.pixels.data() + row * image.width * 4, image.width * 4);
    result.uv = {(result.x + 0.5f) / edge, (result.y + 0.5f) / edge,
                 (result.x + result.width - 0.5f) / edge, (result.y + result.height - 0.5f) / edge};
    return result;
}
}
