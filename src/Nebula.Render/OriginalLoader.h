#pragma once
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <chrono>
#include <algorithm>
#include <cmath>

namespace nebula_original {
using Microsoft::WRL::ComPtr;
inline constexpr uint64_t Budget = 96ull * 1024 * 1024;
struct Failure { HRESULT hr; const wchar_t* operation = L"WIC"; };
inline void Check(HRESULT hr, const wchar_t* operation = L"WIC") { if (FAILED(hr)) throw Failure{hr, operation}; }
struct MetadataWarning {
    HRESULT hr = S_OK;
    const wchar_t* operation = L"";
};
inline bool RecoverableMetadata(HRESULT hr) {
    return hr == HRESULT_FROM_WIN32(ERROR_INVALID_PROFILE) || hr == E_INVALIDARG ||
           hr == WINCODEC_ERR_STREAMREAD || hr == WINCODEC_ERR_BADMETADATAHEADER ||
           hr == WINCODEC_ERR_PROPERTYUNEXPECTEDTYPE || hr == WINCODEC_ERR_UNSUPPORTEDOPERATION ||
           hr == WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
}

struct Request {
    uint64_t scene = 0, serial = 0;
    int id = -1;
    std::wstring path;
    std::array<float, 4> visibleUv{0,0,1,1};
};
struct Image {
    uint64_t scene = 0, serial = 0;
    int id = -1;
    HRESULT hr = S_OK;
    const wchar_t* operation = L"WIC original decode";
    MetadataWarning metadata;
    UINT sourceWidth = 0, sourceHeight = 0, width = 0, height = 0;
    UINT x = 0, y = 0, roiWidth = 0, roiHeight = 0;
    UINT state = 0; // 2 intermediate, 3 full original, 4 original-pixel ROI, 5 failed
    bool complete = true;
    float decodeMs = 0;
    std::vector<unsigned char> pixels;
};

inline ComPtr<IWICBitmapSource> OrientedSrgb(IWICImagingFactory* factory, IWICBitmapFrameDecode* frame,
                                           MetadataWarning* warning = nullptr, bool embedded = true) {
    ComPtr<IWICBitmapSource> source = frame;
    auto recover = [&](const Failure& failure) {
        if (!RecoverableMetadata(failure.hr)) throw failure;
        if (warning && SUCCEEDED(warning->hr)) *warning = {failure.hr, failure.operation};
    };
    if (embedded) {
        try {
            UINT count = 0;
            HRESULT hr = frame->GetColorContexts(0, nullptr, &count);
            if (hr != WINCODEC_ERR_UNSUPPORTEDOPERATION) Check(hr, L"Enumerate embedded color contexts");
            if (SUCCEEDED(hr) && count) {
                if (count > 16) throw Failure{WINCODEC_ERR_BADMETADATAHEADER, L"Too many embedded color contexts"};
                std::array<ComPtr<IWICColorContext>, 16> owners;
                std::array<IWICColorContext*, 16> contexts{};
                for (UINT i = 0; i < count; ++i) {
                    Check(factory->CreateColorContext(&owners[i]));
                    contexts[i] = owners[i].Get();
                }
                UINT actual = 0;
                Check(frame->GetColorContexts(count, contexts.data(), &actual), L"Read embedded color context");
                if (!actual || actual > count) throw Failure{WINCODEC_ERR_BADMETADATAHEADER, L"Inconsistent color context count"};
                UINT selected = 0;
                for (UINT i = 0; i < actual; ++i) {
                    WICColorContextType type;
                    Check(contexts[i]->GetType(&type), L"Read color context type");
                    if (type == WICColorContextProfile) { selected = i; break; }
                }
                ComPtr<IWICColorContext> output;
                Check(factory->CreateColorContext(&output));
                Check(output->InitializeFromExifColorSpace(1));
                ComPtr<IWICColorTransform> transform;
                Check(factory->CreateColorTransformer(&transform));
                Check(transform->Initialize(frame, contexts[selected], output.Get(), GUID_WICPixelFormat32bppBGRA),
                      L"Initialize embedded color transform");
                source = transform;
            }
        } catch (const Failure& failure) { recover(failure); }
    }
    UINT orientation = 1;
    ComPtr<IWICMetadataQueryReader> reader;
    HRESULT metadata = frame->GetMetadataQueryReader(&reader);
    if (SUCCEEDED(metadata)) {
        PROPVARIANT value{};
        HRESULT found = reader->GetMetadataByName(L"/app1/ifd/{ushort=274}", &value);
        if (found == WINCODEC_ERR_PROPERTYNOTFOUND || found == WINCODEC_ERR_PROPERTYNOTSUPPORTED)
            found = reader->GetMetadataByName(L"/ifd/{ushort=274}", &value);
        if (SUCCEEDED(found) && value.vt == VT_UI2) orientation = value.uiVal;
        PropVariantClear(&value);
        if (FAILED(found) && found != WINCODEC_ERR_PROPERTYNOTFOUND && found != WINCODEC_ERR_PROPERTYNOTSUPPORTED &&
            found != WINCODEC_ERR_UNSUPPORTEDOPERATION) recover({found, L"Read EXIF orientation"});
    } else if (metadata != WINCODEC_ERR_UNSUPPORTEDOPERATION && metadata != E_NOINTERFACE) recover({metadata, L"Read frame metadata"});
    const WICBitmapTransformOptions transforms[]{
        WICBitmapTransformRotate0, WICBitmapTransformRotate0, WICBitmapTransformFlipHorizontal,
        WICBitmapTransformRotate180, WICBitmapTransformFlipVertical,
        static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal),
        WICBitmapTransformRotate90,
        static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal),
        WICBitmapTransformRotate270
    };
    if (orientation > 8 || orientation == 0) {
        recover({WINCODEC_ERR_BADMETADATAHEADER, L"Invalid EXIF orientation"});
        orientation = 1;
    }
    if (orientation != 1) {
        ComPtr<IWICBitmapFlipRotator> rotator;
        Check(factory->CreateBitmapFlipRotator(&rotator));
        Check(rotator->Initialize(source.Get(), transforms[orientation]));
        source = rotator;
    }
    return source;
}

inline Image Pixels(IWICImagingFactory* factory, IWICBitmapSource* source, const Request& request,
                    UINT sourceWidth, UINT sourceHeight, const WICRect* roi, UINT maximumEdge, bool upscale = false) {
    Image image;
    image.id = request.id; image.scene = request.scene; image.serial = request.serial;
    image.sourceWidth = sourceWidth; image.sourceHeight = sourceHeight;
    image.roiWidth = sourceWidth; image.roiHeight = sourceHeight;
    ComPtr<IWICBitmapSource> input = source;
    if (roi) {
        ComPtr<IWICBitmapClipper> clipper;
        Check(factory->CreateBitmapClipper(&clipper));
        Check(clipper->Initialize(source, roi));
        input = clipper;
        image.x = roi->X; image.y = roi->Y;
        image.roiWidth = roi->Width; image.roiHeight = roi->Height;
    }
    UINT width, height;
    Check(input->GetSize(&width, &height));
    image.width = width; image.height = height;
    if (maximumEdge && (upscale || std::max(width, height) > maximumEdge)) {
        double scale = static_cast<double>(maximumEdge) / std::max(width, height);
        image.width = std::max(1u, static_cast<UINT>(std::lround(width * scale)));
        image.height = std::max(1u, static_cast<UINT>(std::lround(height * scale)));
        ComPtr<IWICBitmapScaler> scaler;
        Check(factory->CreateBitmapScaler(&scaler));
        Check(scaler->Initialize(input.Get(), image.width, image.height, WICBitmapInterpolationModeFant));
        input = scaler;
    }
    uint64_t bytes = static_cast<uint64_t>(image.width) * image.height * 4;
    if (image.width > 16384 || image.height > 16384 || bytes > Budget) throw Failure{HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE)};
    ComPtr<IWICFormatConverter> converter;
    Check(factory->CreateFormatConverter(&converter));
    Check(converter->Initialize(input.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
          nullptr, 0, WICBitmapPaletteTypeCustom));
    image.pixels.resize(static_cast<size_t>(bytes));
    Check(converter->CopyPixels(nullptr, image.width * 4, static_cast<UINT>(bytes), image.pixels.data()), L"Copy decoded pixels");
    image.state = roi ? 4 : image.width == sourceWidth && image.height == sourceHeight ? 3 : 2;
    return image;
}

inline Image FramePixels(IWICImagingFactory* factory, IWICBitmapFrameDecode* frame, const Request& request,
                         const WICRect* roi, UINT maximumEdge, bool upscale = false) {
    MetadataWarning warning;
    auto source = OrientedSrgb(factory, frame, &warning);
    UINT width, height;
    Check(source->GetSize(&width, &height), L"Read oriented dimensions");
    if (!width || !height || width > 131072 || height > 131072 || static_cast<uint64_t>(width) * height > 1000000000)
        throw Failure{HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE), L"Validate image dimensions"};
    Image image;
    try {
        image = Pixels(factory, source.Get(), request, width, height, roi, maximumEdge, upscale);
    } catch (const Failure& failure) {
        // Some codecs defer color-transform validation until CopyPixels. Retry
        // without that transform; a real pixel-stream failure still propagates.
        if (!RecoverableMetadata(failure.hr)) throw;
        auto plain = OrientedSrgb(factory, frame, nullptr, false);
        image = Pixels(factory, plain.Get(), request, width, height, roi, maximumEdge, upscale);
        warning = {failure.hr, L"Color-managed pixel decode"};
    }
    image.metadata = warning;
    return image;
}

class Loader {
    std::mutex mutex_;
    std::condition_variable wake_, space_;
    std::optional<Request> request_;
    std::optional<Image> ready_;
    std::function<void()> signal_;
    std::atomic<bool> stop_{false}, available_{false};
    std::atomic<uint64_t> serial_{0};
    std::thread thread_;
    void Publish(Image image, std::chrono::steady_clock::time_point start) {
        image.decodeMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::unique_lock lock(mutex_);
        space_.wait(lock, [&] { return stop_ || image.serial != serial_ || !ready_; });
        if (stop_ || image.serial != serial_) return;
        ready_ = std::move(image);
        available_ = true;
        lock.unlock();
        signal_();
    }
    void Run() noexcept {
        HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            Request request;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [&] { return stop_ || request_.has_value(); });
                if (stop_) break;
                request = std::move(*request_);
                request_.reset();
            }
            auto start = std::chrono::steady_clock::now();
            try {
                Check(com);
                ComPtr<IWICImagingFactory> factory;
                Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
                ComPtr<IWICBitmapDecoder> decoder;
                Check(factory->CreateDecoderFromFilename(request.path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder), L"Open original");
                ComPtr<IWICBitmapFrameDecode> frame;
                Check(decoder->GetFrame(0, &frame));
                auto source = OrientedSrgb(factory.Get(), frame.Get());
                UINT width, height;
                Check(source->GetSize(&width, &height));
                if (!width || !height || width > 131072 || height > 131072 ||
                    static_cast<uint64_t>(width) * height > 1000000000) throw Failure{HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE)};
                bool fullFits = width <= 16384 && height <= 16384 && static_cast<uint64_t>(width) * height * 4 <= Budget;
                auto uv = request.visibleUv;
                UINT x = static_cast<UINT>(std::floor(uv[0] * width / 512)) * 512;
                UINT y = static_cast<UINT>(std::floor(uv[1] * height / 512)) * 512;
                UINT right = std::min(width, static_cast<UINT>(std::ceil(uv[2] * width / 512)) * 512);
                UINT bottom = std::min(height, static_cast<UINT>(std::ceil(uv[3] * height / 512)) * 512);
                bool roiFits = right > x && bottom > y && right - x <= 16384 && bottom - y <= 16384 &&
                               static_cast<uint64_t>(right - x) * (bottom - y) * 4 <= Budget;
                if (std::max(width, height) > 2048 && (fullFits || uv == std::array<float, 4>{0,0,1,1})) {
                    auto intermediate = FramePixels(factory.Get(), frame.Get(), request, nullptr, 2048);
                    intermediate.complete = !fullFits && !roiFits;
                    Publish(std::move(intermediate), start);
                    if (stop_ || request.serial != serial_) continue;
                }
                if (fullFits) {
                    Publish(FramePixels(factory.Get(), frame.Get(), request, nullptr, 0), start);
                } else {
                    if (roiFits) {
                        WICRect roi{static_cast<INT>(x), static_cast<INT>(y), static_cast<INT>(right - x), static_cast<INT>(bottom - y)};
                        Publish(FramePixels(factory.Get(), frame.Get(), request, &roi, 0), start);
                    } else if (uv != std::array<float, 4>{0,0,1,1}) {
                        Publish(FramePixels(factory.Get(), frame.Get(), request, nullptr, 2048), start);
                    }
                }
            } catch (const Failure& error) {
                Image image; image.id = request.id; image.scene = request.scene; image.serial = request.serial;
                image.hr = error.hr; image.operation = error.operation; image.state = 5;
                Publish(std::move(image), start);
            } catch (const std::bad_alloc&) {
                Image image; image.id = request.id; image.scene = request.scene; image.serial = request.serial;
                image.hr = E_OUTOFMEMORY; image.state = 5;
                Publish(std::move(image), start);
            } catch (...) {
                Image image; image.id = request.id; image.scene = request.scene; image.serial = request.serial;
                image.hr = E_FAIL; image.state = 5;
                Publish(std::move(image), start);
            }
        }
        if (SUCCEEDED(com)) CoUninitialize();
    }
public:
    explicit Loader(std::function<void()> signal) : signal_(std::move(signal)), thread_([this] { Run(); }) {}
    ~Loader() {
        stop_ = true;
        { std::lock_guard lock(mutex_); wake_.notify_all(); space_.notify_all(); }
        if (thread_.joinable()) thread_.join();
    }
    uint64_t RequestImage(uint64_t scene, int id, std::wstring path, std::array<float, 4> uv = {0,0,1,1}) {
        std::lock_guard lock(mutex_);
        uint64_t serial = ++serial_;
        request_ = Request{scene, serial, id, std::move(path), uv};
        ready_.reset(); available_ = false;
        wake_.notify_one(); space_.notify_all();
        return serial;
    }
    void Cancel() {
        std::lock_guard lock(mutex_);
        ++serial_; request_.reset(); ready_.reset(); available_ = false;
        space_.notify_all();
    }
    bool Ready() const { return available_; }
    std::optional<Image> Take() {
        std::lock_guard lock(mutex_);
        auto result = std::move(ready_);
        ready_.reset(); available_ = false; space_.notify_all();
        return result;
    }
};
}
