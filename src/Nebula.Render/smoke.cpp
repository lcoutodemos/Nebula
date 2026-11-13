#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "NebulaRender.h"
#include "Shaders.h"
#include "MosaicAverages.h"
#include "RenderLimits.h"
#include <windows.h>
#include <dxgi1_3.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <d3dcompiler.h>
#include <microsoft.ui.xaml.media.dxinterop.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>
#include <windows.data.json.h>
#include <roapi.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <random>
#include <map>
#include <iomanip>
#include <unordered_set>
#include "ShaderTests.h"
#include "ReportTests.h"
#include "DepthShaderTests.h"
#include "ThumbnailDecoder.h"
#include "LodTests.h"

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void Hr(int result, const char* operation) {
    if (FAILED(static_cast<HRESULT>(result))) {
        std::cerr << operation << " HRESULT 0x" << std::hex << static_cast<unsigned>(result) << std::dec << '\n';
        throw std::runtime_error(operation);
    }
}

void CompileShaders() {
    for (auto source : {PhotoShader, GlassShader}) {
        std::vector<const char*> entries = source == PhotoShader
            ? std::vector<const char*>{"VS", "PS"}
            : std::vector<const char*>{"FullVS", "DownsamplePS", "BlurPS", "CompositePS"};
        for (auto entry : entries) {
            ComPtr<ID3DBlob> code, errors;
            HRESULT hr = D3DCompile(source, strlen(source), "Nebula shader validation", nullptr, nullptr,
                entry, strstr(entry, "VS") ? "vs_5_0" : "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
            if (FAILED(hr) && errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer()) << '\n';
            Hr(hr, entry);
        }
    }
}

void MosaicAverageTests() {
    std::vector<unsigned char> image(128 * 128 * 4);
    for (int y = 0; y < 128; ++y) for (int x = 0; x < 128; ++x) {
        auto* pixel = image.data() + (y * 128 + x) * 4;
        pixel[0] = pixel[2] = 0; pixel[1] = pixel[3] = 255;
        if (y >= 32 && y < 96) pixel[0] = pixel[1] = pixel[2] = ((x + y) & 1) ? 255 : 0;
    }
    auto pyramid = BuildMosaicAverages(image, 128, 0, 32, 128, 64, 2);
    Require(pyramid.size() == 1876, "Mosaic pyramid plus 128-cell art has incorrect byte budget");
    for (size_t i = 0; i < pyramid.size(); i += 4) {
        Require(pyramid[i] >= 187 && pyramid[i] <= 188 && pyramid[i] == pyramid[i + 1] &&
                pyramid[i] == pyramid[i + 2] && pyramid[i + 3] == 255,
                "Mosaic must average black/white to linear-light gray, excluding coloured aspect padding");
    }
    for (int y = 32; y < 96; ++y) for (int x = 0; x < 128; ++x)
        image[(y * 128 + x) * 4 + 3] = ((x + y) & 1) ? 0 : 255;
    pyramid = BuildMosaicAverages(image, 128, 0, 32, 128, 64, 2);
    Require(pyramid[1360] == 0 && pyramid[1361] == 0 && pyramid[1362] == 0 &&
            pyramid[1363] >= 127 && pyramid[1363] <= 128, "Mosaic averages must weight transparency rather than leak transparent RGB");
    for (int y = 0; y < 128; ++y) for (int x = 0; x < 128; ++x) {
        auto* pixel = image.data() + (y * 128 + x) * 4;
        pixel[0] = static_cast<unsigned char>(x * 2); pixel[1] = 0;
        pixel[2] = static_cast<unsigned char>(y * 2); pixel[3] = 255;
    }
    auto portrait = BuildMosaicAverages(image, 128, 32, 0, 64, 128, 0.5f);
    Require(portrait[1364 + 7 * 4] > portrait[1364] + 100 &&
            portrait[1364 + 15 * 8 * 4 + 2] > portrait[1364 + 2] + 200,
            "Portrait 128-cell art must use the canonical row-major 8x16 hierarchy");
    std::cout << "PASS actual decoder mosaic builder: 16/8/4/2/1 block AVERAGES, sRGB-linear conversion,\n"
              << "     padding exclusion, alpha weighting, 1876-byte cache record including optional art (not nearest samples)\n";
}

class TestPanel final : public ISwapChainPanelNative {
    std::atomic<ULONG> refs_{1};
public:
    ComPtr<IDXGISwapChain2> chain;
    unsigned attaches = 0, detaches = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(ISwapChainPanelNative)) return E_NOINTERFACE;
        *value = static_cast<ISwapChainPanelNative*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = --refs_;
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE SetSwapChain(IDXGISwapChain* value) override {
        chain.Reset();
        if (!value) { ++detaches; return S_OK; }
        ++attaches;
        return value->QueryInterface(IID_PPV_ARGS(&chain));
    }
};

class Handle {
public:
    void* value = nullptr;
    ~Handle() { nb_destroy(value); }
};

void WriteThumbnail(const std::filesystem::path& path) {
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    constexpr int width = 256, height = 128;
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + width * height * 4;
    info.biSize = sizeof(info);
    info.biWidth = width;
    info.biHeight = height;
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    std::vector<unsigned char> pixels(width * height * 4);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        size_t offset = (static_cast<size_t>(y) * width + x) * 4;
        pixels[offset] = static_cast<unsigned char>(x);
        pixels[offset + 1] = static_cast<unsigned char>(y * 2);
        pixels[offset + 2] = 220;
        pixels[offset + 3] = 255;
    }
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file), sizeof(file));
    output.write(reinterpret_cast<const char*>(&info), sizeof(info));
    output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    Require(output.good(), "Write generated thumbnail");
}

NbStats Stats(void* renderer) {
    NbStats stats{sizeof(NbStats)};
    auto hr = nb_get_stats(renderer, &stats);
    if (FAILED(hr)) {
        wchar_t error[2048]{};
        nb_get_error(renderer, error, ARRAYSIZE(error));
        std::wcerr << error << '\n';
        Hr(hr, "Read renderer stats");
    }
    Require(stats.resident <= nebula_limits::PoolSlots, "Resident pool overflow");
    Require(stats.pending <= nebula_limits::RequestLimit + nebula_limits::ResultLimit + nebula_limits::MosaicJobs + 2,
            "Decode queue/buffer budget overflow");
    return stats;
}

class MemoryBudgetProbe {
    ComPtr<IDXGIAdapter3> adapter_;
    uint64_t peakPrivate_ = 0, peakGpu_ = 0, peakCombined_ = 0;
    uint64_t budget_;
public:
    explicit MemoryBudgetProbe(IDXGISwapChain2* chain, uint64_t budget = nebula_limits::FiveThousandMemoryBudget) : budget_(budget) {
        ComPtr<IDXGIDevice> device;
        ComPtr<IDXGIAdapter> adapter;
        Hr(chain->GetDevice(IID_PPV_ARGS(&device)), "Get renderer DXGI device for budget measurement");
        Hr(device->GetAdapter(&adapter), "Get renderer adapter for budget measurement");
        Hr(adapter.As(&adapter_), "Query video memory accounting");
        static_assert(nebula_limits::ThumbEdge == 256);
        static_assert(nebula_limits::ImagePoolBytes == 156850176);
        Require(nebula_limits::ImagePoolBytes + nebula_limits::MosaicPoolBytes <
                nebula_limits::FiveThousandMemoryBudget, "Declared pool exceeds 5k budget");
    }

    void Sample() {
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        Require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) != FALSE,
                "Read process private-memory commitment");
        DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonlocal{};
        Hr(adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local), "Read local GPU usage");
        Hr(adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal), "Read nonlocal GPU usage");
        uint64_t gpu = local.CurrentUsage + nonlocal.CurrentUsage;
        uint64_t combined = memory.PrivateUsage + gpu;
        peakPrivate_ = std::max(peakPrivate_, static_cast<uint64_t>(memory.PrivateUsage));
        peakGpu_ = std::max(peakGpu_, gpu);
        peakCombined_ = std::max(peakCombined_, combined);
        if (combined >= budget_)
            std::cerr << "Memory budget: CPU private=" << memory.PrivateUsage << " GPU local=" << local.CurrentUsage
                      << " GPU nonlocal=" << nonlocal.CurrentUsage << " combined=" << combined << '\n';
        Require(combined < budget_, "CPU-private plus GPU-memory usage exceeds the measured scene budget");
    }

    void Report(size_t items = 5000) {
        Sample();
        std::cout << "PASS " << items << "-item memory: BC7 full-mip pools 64/128/256/512px x 8192/1024/512/128"
                  << " = " << nebula_limits::ImagePoolBytes << " bytes; normal mosaics=" << items * nebula_limits::ResidentMosaicBytes
                  << " bytes, optional Pixels art=" << items * nebula_limits::ArtBytesPerSlice
                  << " bytes; sampled peak CPU-private=" << peakPrivate_ << ", GPU-local+nonlocal=" << peakGpu_
                  << ", combined=" << peakCombined_ << " < " << budget_ << " bytes\n";
    }
    uint64_t Peak() const { return peakCombined_; }
};

NbLodStats Lod(void* renderer) {
    NbLodStats stats{sizeof(NbLodStats)};
    Hr(nb_get_lod_stats(renderer, &stats), "Read LOD/performance snapshot");
    Require(stats.thumbnailResident <= nebula_limits::ThumbnailSlots &&
            stats.previewResident <= nebula_limits::PreviewSlots, "Streamed tier pool overflow");
    Require(stats.mosaicReady <= stats.items, "Scene-resident mosaic count overflow");
    return stats;
}
NbStats Wait(void* renderer, const std::function<bool(const NbStats&)>& predicate, const char* message) {
    auto end = std::chrono::steady_clock::now() + 25s;
    while (std::chrono::steady_clock::now() < end) {
        auto stats = Stats(renderer);
        if (predicate(stats)) return stats;
        std::this_thread::sleep_for(10ms);
    }
    auto stats = Stats(renderer);
    std::cerr << message << ": items=" << stats.items << " resident=" << stats.resident
              << " pending=" << stats.pending << " failed=" << stats.failed << " frames=" << stats.frames
              << " fps=" << stats.fps << '\n';
    throw std::runtime_error(message);
}

void Idle(void* renderer) {
    uint64_t previousFrame = UINT64_MAX;
    auto quietSince = std::chrono::steady_clock::now();
    auto settled = Wait(renderer, [&](const NbStats& s) {
        auto now = std::chrono::steady_clock::now();
        if (s.pending || s.fps != 0 || s.frames != previousFrame) quietSince = now;
        previousFrame = s.frames;
        return s.pending == 0 && s.fps == 0 && now - quietSince >= 200ms;
    }, "Renderer did not reach decode-drained, zero-FPS idle");
    for (int i = 0; i < 5; ++i) {
        std::this_thread::sleep_for(40ms);
        auto sample = Stats(renderer);
        Require(sample.frames == settled.frames, "Stats polling must not cause idle presents");
        Require(sample.pending == 0, "Idle was published with decode work still pending");
        Require(sample.fps == 0, "Idle FPS must remain zero");
    }
}

void IdleFpsRegression(void* renderer) {
    Idle(renderer);
    auto start = std::chrono::steady_clock::now();
    uint64_t frame = Stats(renderer).frames;
    Hr(nb_set_theme(renderer, 1), "First one-frame FPS pulse");
    auto first = Wait(renderer, [frame](const NbStats& s) { return s.frames > frame; }, "Present first FPS pulse");
    Hr(nb_set_theme(renderer, 0), "Second one-frame FPS pulse");
    auto second = Wait(renderer, [first](const NbStats& s) { return s.frames > first.frames; }, "Present second FPS pulse");
    std::this_thread::sleep_for(40ms);
    auto idle = Stats(renderer);
    Require(idle.frames == second.frames && idle.pending == 0, "Single-frame pulse unexpectedly left rendering work");
    if (idle.fps != 0) std::cerr << "Settled FPS regression: frames=" << second.frames << "->" << idle.frames
        << " pending=" << idle.pending << " fps=" << idle.fps << " after "
        << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() << "ms\n";
    Require(idle.fps == 0, "Completed event-driven frames must publish zero idle FPS without a 500ms grace period");
    Idle(renderer);
    std::cout << "PASS scheduler idle FPS: completed frames=" << first.frames << "," << second.frames
              << "; stable=" << idle.frames << "; pending=0, fps=0 without delayed timeout\n";
}

int HorizontalFootprint(void* renderer, int id) {
    int pixels = 0;
    for (int x = 0; x < 960; ++x) {
        int hit = -1;
        Hr(nb_pick(renderer, static_cast<float>(x), 360, &hit), "Measure photograph footprint");
        pixels += hit == id;
    }
    return pixels;
}

void RefinementTests(void* renderer) {
    Require(sizeof(NbGlassRect) == 24, "Glass ABI must be six packed floats");
    Require(FAILED(nb_hover(renderer, 999)), "Reject unknown hover");
    Require(FAILED(nb_focus(renderer, -1)), "Reject invalid focus");
    Require(FAILED(nb_set_reduced_motion(renderer, 2)), "Reject invalid motion mode");
    Require(FAILED(nb_set_pixels(renderer, -1)) && FAILED(nb_set_pixels(renderer, 6)), "Reject invalid pixel-art level");
    Require(FAILED(nb_set_glass(renderer, nullptr, 1)), "Reject null glass array");
    Require(FAILED(nb_set_glass(renderer, nullptr, -1)), "Reject negative glass count");
    NbGlassRect rectangles[9]{};
    for (int i = 0; i < 9; ++i) rectangles[i] = {static_cast<float>(i * 90), 28, 80, 160, 14, 0.30f};
    Require(FAILED(nb_set_glass(renderer, rectangles, 9)), "Reject more than eight glass rectangles");
    NbGlassRect bad = rectangles[0];
    bad.x = NAN;
    Require(FAILED(nb_set_glass(renderer, &bad, 1)), "Reject nonfinite glass coordinate");
    bad = rectangles[0]; bad.width = -1;
    Require(FAILED(nb_set_glass(renderer, &bad, 1)), "Reject negative glass dimensions");
    bad = rectangles[0]; bad.tint = 1.1f;
    Require(FAILED(nb_set_glass(renderer, &bad, 1)), "Reject out-of-range glass tint");
    Hr(nb_set_glass(renderer, rectangles, 8), "Composite eight glass rectangles");
    Idle(renderer);
    IdleFpsRegression(renderer);
    auto frames = Stats(renderer).frames;
    for (int i = 0; i < 6; ++i) Hr(nb_set_glass(renderer, rectangles, 8), "Resubmit identical glass rectangles");
    std::this_thread::sleep_for(100ms);
    Require(Stats(renderer).frames == frames, "Unchanged LayoutUpdated rectangles woke renderer");
    // Only the first two entries are valid: count must not accidentally mean floats/bytes.
    rectangles[0] = {-24, -12, 210, 280, 24, 0.24f};
    rectangles[1] = {760, 650, 230, 150, 30, 0.32f};
    rectangles[2].x = NAN;
    Hr(nb_set_glass(renderer, rectangles, 2), "Clip partially offscreen panels and honor count");
    Idle(renderer);
    Hr(nb_set_glass(renderer, nullptr, 0), "Bypass backdrop blur");
    Idle(renderer);

    Require(sizeof(NbGlassPanel) == 28 && offsetof(NbGlassPanel, opacity) == 24,
            "Animated glass ABI must be seven floats");
    NbGlassPanel panels[9]{};
    for (int i = 0; i < 9; ++i)
        panels[i] = {static_cast<float>(i * 100 - 15), 35, 130, 180, 18, 0.55f, 1};
    Require(FAILED(nb_set_glass_panels(renderer, nullptr, 1)), "Reject null animated-glass array");
    Require(FAILED(nb_set_glass_panels(renderer, panels, -1)) &&
            FAILED(nb_set_glass_panels(renderer, panels, 9)), "Reject invalid animated-glass count");
    for (int field = 0; field < 7; ++field) {
        NbGlassPanel invalid = panels[0];
        switch (field) {
            case 0: invalid.x = NAN; break;
            case 1: invalid.y = NAN; break;
            case 2: invalid.width = NAN; break;
            case 3: invalid.height = NAN; break;
            case 4: invalid.radius = NAN; break;
            case 5: invalid.tint = NAN; break;
            default: invalid.opacity = NAN; break;
        }
        Require(FAILED(nb_set_glass_panels(renderer, &invalid, 1)), "Reject any nonfinite animated-glass field");
    }
    for (float opacity : {-0.1f, 1.1f, INFINITY}) {
        NbGlassPanel invalid = panels[0];
        invalid.opacity = opacity;
        Require(FAILED(nb_set_glass_panels(renderer, &invalid, 1)), "Reject out-of-range glass opacity");
    }
    NbGlassPanel invalid = panels[0]; invalid.width = -1;
    Require(FAILED(nb_set_glass_panels(renderer, &invalid, 1)), "Reject negative animated-panel width");
    invalid = panels[0]; invalid.radius = -1;
    Require(FAILED(nb_set_glass_panels(renderer, &invalid, 1)), "Reject negative animated-panel radius");
    Hr(nb_set_glass_panels(renderer, panels, 8), "Accept eight animated panels");
    Idle(renderer);
    panels[0].opacity = 0.5f;
    panels[1].opacity = 0;
    panels[2].opacity = NAN;
    Hr(nb_set_glass_panels(renderer, panels, 2), "Honor partial animated-panel count");
    Idle(renderer);
    frames = Stats(renderer).frames;
    for (int i = 0; i < 6; ++i)
        Hr(nb_set_glass_panels(renderer, panels, 2), "Deduplicate complete bounds/opacity snapshot");
    std::this_thread::sleep_for(100ms);
    Require(Stats(renderer).frames == frames, "Identical animated-panel snapshot caused idle rendering");
    for (float opacity : {0.0f, 0.5f, 1.0f, 0.5f, 0.0f}) {
        panels[0].x = -15 + (1 - opacity) * -24;
        panels[0].opacity = opacity;
        frames = Stats(renderer).frames;
        Hr(nb_set_glass_panels(renderer, panels, 1), "Submit one externally-clocked panel state");
        auto applied = Wait(renderer, [frames](const NbStats& s) { return s.frames > frames; }, "Present externally-clocked panel state");
        std::this_thread::sleep_for(80ms);
        Require(Stats(renderer).frames == applied.frames && Stats(renderer).fps == 0,
                "Native started an independent panel tween after the supplied snapshot");
    }
    panels[0] = {35, 35, 220, 300, 18, 0.55f, 1};
    Hr(nb_set_glass_panels(renderer, panels, 1), "Set opaque panel through new API");
    Idle(renderer);
    NbGlassRect legacy{35, 35, 220, 300, 18, 0.55f};
    frames = Stats(renderer).frames;
    Hr(nb_set_glass(renderer, &legacy, 1), "Legacy glass must map to opacity one");
    std::this_thread::sleep_for(80ms);
    Require(Stats(renderer).frames == frames, "Legacy opacity-one glass did not match the new snapshot");
    Hr(nb_set_glass_panels(renderer, nullptr, 0), "Release animated-panel targets");
    Idle(renderer);

    int initial = HorizontalFootprint(renderer, 17);
    frames = Stats(renderer).frames;
    Hr(nb_hover(renderer, 17), "Hover photograph");
    std::this_thread::sleep_for(40ms);
    Require(HorizontalFootprint(renderer, 17) == initial, "Hover lifted before its 100ms dwell");
    Idle(renderer);
    Require(Stats(renderer).frames > frames + 3, "Hover did not fade");
    Require(HorizontalFootprint(renderer, 17) > initial, "Hover did not lift rounded tile");
    frames = Stats(renderer).frames;
    Hr(nb_hover(renderer, 17), "Resubmit same hover");
    std::this_thread::sleep_for(100ms);
    Require(Stats(renderer).frames == frames, "Unchanged hover woke renderer");
    Hr(nb_hover(renderer, -1), "Clear hover");
    Idle(renderer);
    frames = Stats(renderer).frames;
    Hr(nb_select(renderer, 17), "Ease click selection");
    Wait(renderer, [frames](const NbStats& s) { return s.frames > frames; }, "Present first selection frame");
    int earlySelection = HorizontalFootprint(renderer, 17);
    std::this_thread::sleep_for(220ms);
    int fullSelection = HorizontalFootprint(renderer, 17);
    Require(fullSelection > initial * 1.30f, "Selection did not lift photograph by the expected envelope");
    Require(earlySelection < fullSelection - 15, "Selection size popped instead of easing over 160ms");
    Hr(nb_select(renderer, -1), "Ease deselection");
    Idle(renderer);
    for (int level = 1; level <= 5; ++level) {
        frames = Stats(renderer).frames;
        Hr(nb_set_pixels(renderer, level), "Apply pixel mosaic level");
        Wait(renderer, [frames](const NbStats& s) { return s.frames > frames; }, "Present pixel mosaic");
        Require(HorizontalFootprint(renderer, 17) == initial, "Mosaic art changed geometry/picking");
    }
    Hr(nb_set_pixels(renderer, 0), "Restore resolved photographs");

    frames = Stats(renderer).frames;
    Hr(nb_focus(renderer, 17), "Smooth focus without changing orbit");
    Idle(renderer);
    int focused = HorizontalFootprint(renderer, 17);
    Require(Stats(renderer).frames > frames + 3, "Focus did not animate");
    Require(focused != initial, "Focus did not reframe photograph");
    Hr(nb_select(renderer, -1), "Clear selection independently of camera history");
    Hr(nb_back(renderer), "Restore previous camera");
    Idle(renderer);
    Require(std::abs(HorizontalFootprint(renderer, 17) - initial) <= 2, "Back did not restore exact framing");

    frames = Stats(renderer).frames;
    Hr(nb_set_reduced_motion(renderer, 1), "Enable reduced motion");
    Wait(renderer, [frames](const NbStats& s) { return s.frames > frames; }, "Apply reduced-motion setting");
    frames = Stats(renderer).frames;
    Hr(nb_focus(renderer, 17), "Immediate reduced-motion focus");
    Wait(renderer, [frames](const NbStats& s) { return s.frames > frames; }, "Present immediate focus");
    int immediate = HorizontalFootprint(renderer, 17);
    Require(std::abs(immediate - focused) <= 2, "Reduced-motion focus was not immediate");
    frames = Stats(renderer).frames;
    std::this_thread::sleep_for(150ms);
    Require(Stats(renderer).frames == frames, "Reduced motion left a camera animation running");
    Hr(nb_back(renderer), "Immediate reduced-motion back");
    Hr(nb_set_reduced_motion(renderer, 0), "Restore normal motion");
    Idle(renderer);
}

struct PhotoBounds {
    int left = 960, right = -1, top = 720, bottom = -1;
    bool Found() const { return right >= left; }
    float CenterX() const { return (left + right) * 0.5f; }
    int Width() const { return Found() ? right - left + 1 : 0; }
};

std::array<PhotoBounds, 3> SearchBounds(void* renderer) {
    std::array<PhotoBounds, 3> bounds;
    for (int y = 0; y < 720; y += 6) for (int x = 0; x < 960; x += 6) {
        int id = -1;
        Hr(nb_pick(renderer, static_cast<float>(x), static_cast<float>(y), &id), "Measure search arrangement");
        int index = id == 101 ? 0 : id == 202 ? 1 : id == 303 ? 2 : -1;
        if (index >= 0) {
            auto& b = bounds[index];
            b.left = std::min(b.left, x); b.right = std::max(b.right, x);
            b.top = std::min(b.top, y); b.bottom = std::max(b.bottom, y);
        }
    }
    return bounds;
}

void SearchTests(void* renderer, const wchar_t* path) {
    Require(sizeof(NbResult) == 20, "Search result ABI must be 20 bytes");
    NbItem scene[]{
        {101, -3, 0, 0, 1, 0.7f, 0.3f, 0.2f, path},
        {202,  0, 0, 0, 1, 0.3f, 0.7f, 0.2f, path},
        {303,  3, 0, 0, 1, 0.2f, 0.3f, 0.7f, path}
    };
    Hr(nb_set_scene(renderer, scene, 3), "Create search fixture");
    Wait(renderer, [](const NbStats& s) { return s.items == 3 && s.resident == 3 && !s.pending; }, "Load search fixture");
    Idle(renderer);
    auto original = SearchBounds(renderer);
    Require(original[0].Found() && original[1].Found() && original[2].Found(), "Base search fixture has missing photographs");
    Require(original[0].CenterX() < original[2].CenterX(), "Base fixture order is incorrect");
    NbResult invalid{999, 0, 0, 0, 1};
    Require(FAILED(nb_set_results(renderer, &invalid, 1)), "Reject unknown result ID");
    invalid = {101, NAN, 0, 0, 1};
    Require(FAILED(nb_set_results(renderer, &invalid, 1)), "Reject nonfinite result position");
    invalid = {101, 0, 0, 0, INFINITY};
    Require(FAILED(nb_set_results(renderer, &invalid, 1)), "Reject nonfinite result weight");
    NbResult duplicate[]{{101, 0, 0, 0, 1}, {101, 1, 0, 0, 1}};
    Require(FAILED(nb_set_results(renderer, duplicate, 2)), "Reject duplicate result IDs");
    Require(FAILED(nb_set_results(renderer, nullptr, 1)) && FAILED(nb_set_results(renderer, nullptr, -1)), "Reject invalid result array/count");

    NbResult matches[]{{303, -1.5f, 0, 0, 0}, {101, 1.5f, 0, 0, 1}};
    auto before = Stats(renderer).frames;
    Hr(nb_set_results(renderer, matches, 2), "Reorder metadata search results by stable IDs");
    Wait(renderer, [before](const NbStats& s) { return s.frames > before; }, "Present start of search morph");
    auto beginning = SearchBounds(renderer);
    Require(beginning[0].CenterX() < beginning[2].CenterX(), "Search jumped immediately instead of morphing from current positions");
    Idle(renderer);
    auto gathered = SearchBounds(renderer);
    IdleFpsRegression(renderer);
    Require(gathered[0].Found() && gathered[2].Found() && !gathered[1].Found(), "Search did not preserve matched photos and recede unrelated context");
    Require(gathered[2].CenterX() < gathered[0].CenterX(), "Search did not actually reorder results");
    Require(gathered[0].Width() > gathered[2].Width() * 1.2f, "Result confidence did not affect result scale");
    Require(Stats(renderer).frames > before + 8, "Search morph did not animate");

    float newBase[]{3, 0, 0, 0, 0, 0, -3, 0, 0};
    Hr(nb_set_positions(renderer, newBase, 3), "Update base layout while search remains active");
    Idle(renderer);
    auto stillGathered = SearchBounds(renderer);
    Require(std::abs(stillGathered[0].CenterX() - gathered[0].CenterX()) <= 6 &&
            std::abs(stillGathered[2].CenterX() - gathered[2].CenterX()) <= 6,
            "Base layout update displaced active search result positions");
    unsigned char mask[]{1, 0, 1};
    Hr(nb_set_filter(renderer, mask, 3), "Apply structural filter independently");
    Hr(nb_set_results(renderer, nullptr, 0), "Enter explicit empty-result search");
    Idle(renderer);
    auto empty = SearchBounds(renderer);
    Require(!empty[0].Found() && !empty[1].Found() && !empty[2].Found() && Stats(renderer).items == 3,
            "Empty search cleared the library or restored the base instead of receding all context");
    Hr(nb_clear_results(renderer), "Clear search and restore latest base");
    Idle(renderer);
    auto restored = SearchBounds(renderer);
    Require(restored[0].Found() && restored[2].Found() && !restored[1].Found(), "Clear search changed the independent structural filter");
    Require(restored[2].CenterX() < restored[0].CenterX(), "Clear search restored stale rather than latest base positions");
    mask[1] = 1;
    before = Stats(renderer).frames;
    Hr(nb_set_filter(renderer, mask, 3), "Restore structural visibility");
    Wait(renderer, [before](const NbStats& s) { return s.frames > before; }, "Present restored visibility");
    Require(SearchBounds(renderer)[1].Found(), "Restored base photograph remained hidden");

    NbResult outbound[]{{101, -2, 0, 0, 1}, {303, 2, 0, 0, 1}};
    NbResult inbound[]{{101, 2, 0, 0, 1}, {303, -2, 0, 0, 1}};
    Hr(nb_set_results(renderer, outbound, 2), "Begin interruptible search morph");
    std::this_thread::sleep_for(70ms);
    auto reversing = SearchBounds(renderer);
    before = Stats(renderer).frames;
    Hr(nb_set_results(renderer, inbound, 2), "Reverse search before morph completes");
    Wait(renderer, [before](const NbStats& s) { return s.frames > before; }, "Present reversed search");
    auto reversed = SearchBounds(renderer);
    Require(reversing[0].Found() && reversed[0].Found() &&
            std::abs(reversing[0].CenterX() - reversed[0].CenterX()) < 150,
            "Reversing search jumped to an old target rather than starting from current state");
    Idle(renderer);
    Hr(nb_clear_results(renderer), "Restore library after interruption");
    Idle(renderer);
    auto selectionBaseline = SearchBounds(renderer);
    Hr(nb_select(renderer, 101), "Select without framing");
    Idle(renderer);
    auto selectedBeforeQuery = SearchBounds(renderer);
    Require(std::abs(selectedBeforeQuery[0].CenterX() - selectionBaseline[0].CenterX()) <= 6,
            "Single-click selection moved the camera");
    NbResult oneMatch{303, 0, 0, 0, 1};
    Hr(nb_set_results(renderer, &oneMatch, 1), "Search with an existing nonmatching selection");
    Idle(renderer);
    Hr(nb_select(renderer, 202), "Retain inspector-selected nonmatch during search");
    std::this_thread::sleep_for(200ms);
    Hr(nb_set_results(renderer, &oneMatch, 1), "Refresh results without changing selection identity");
    Idle(renderer);
    Hr(nb_clear_results(renderer), "Clear query without restoring stale selection");
    Idle(renderer);
    auto selectedAfterClear = SearchBounds(renderer);
    Require(selectedAfterClear[1].Width() > selectionBaseline[1].Width() * 1.25f,
            "Results/clear lost current selection or restored a stale inspector identity");
    Hr(nb_focus(renderer, 303), "Explicitly frame another photo");
    Idle(renderer);
    Hr(nb_back(renderer), "Restore camera while preserving newly selected photo");
    Idle(renderer);
    Require(SearchBounds(renderer)[2].Width() > selectionBaseline[2].Width() * 1.25f,
            "Camera back changed the selection identity");
    Hr(nb_select(renderer, -1), "Clear retained test selection");
    Idle(renderer);
    std::cout << "PASS search: real ID-based reordering, confidence scale, 280ms morph/reversal, empty-vs-clear,\n"
              << "     latest-base restoration, independent filters, retained inspector selection/camera-only back;\n"
              << "     single-click camera neutrality; NbResult=" << sizeof(NbResult) << '\n';
}

NbView View(void* renderer) {
    NbView view{sizeof(NbView)};
    Hr(nb_get_view(renderer, &view), "Read presented camera snapshot");
    Require(view.width > 0 && view.height > 0, "No presented camera snapshot");
    return view;
}

std::array<float, 3> Project(const NbView& view, const std::array<float, 3>& position) {
    float world[]{position[0], position[1], position[2], 1}, clip[4]{};
    for (int row = 0; row < 4; ++row) for (int col = 0; col < 4; ++col)
        clip[row] += view.viewProj[row * 4 + col] * world[col];
    Require(clip[3] > 0, "Projected test photo is behind the camera");
    return {(clip[0] / clip[3] + 1) * view.width / 2,
            (1 - clip[1] / clip[3]) * view.height / 2, clip[3]};
}

int ProjectedWidth(void* renderer, int id, const std::array<float, 3>& position) {
    auto view = View(renderer);
    auto center = Project(view, position);
    int left = static_cast<int>(view.width), right = -1;
    for (int x = 0; x < static_cast<int>(view.width); ++x) {
        int hit, tier;
        Hr(nb_pick_lod(renderer, x + 0.5f, center[1], &hit, &tier), "Measure highlighted photo at current LOD");
        if (hit == id) { left = std::min(left, x); right = x; }
    }
    Require(right >= left, "Known photo not found at projected centre");
    Require(std::abs((left + right + 1) * 0.5f - center[0]) <= 1,
            "Current view does not project to the displayed photo centre within 1 px");
    return right - left + 1;
}

void HighlightTests(void* renderer, const wchar_t* path) {
    Require(sizeof(NbLine) == 12 && sizeof(NbView) == 88, "Highlight/view ABI mismatch");
    NbItem scene[]{
        {10, -3, 0, 0, 1, 0.5f, 0.5f, 0.5f, path},
        {20, 3, 0, 0, 1, 0.5f, 0.5f, 0.5f, path},
        {30, 0, 3, 0, 1, 0.5f, 0.5f, 0.5f, path}
    };
    Hr(nb_set_scene(renderer, scene, 3), "Set highlight fixture");
    Wait(renderer, [](const NbStats& s) { return s.items == 3 && s.resident == 3 && !s.pending; }, "Load highlight fixture");
    Idle(renderer);
    uint32_t ids[]{20, 30};
    float weights[]{1, 0.55f};
    Require(FAILED(nb_set_highlight(renderer, nullptr, weights, 1)) &&
            FAILED(nb_set_highlight(renderer, ids, nullptr, 1)), "Reject null highlight data");
    uint32_t duplicate[]{20, 20}, unknown[]{999};
    Require(FAILED(nb_set_highlight(renderer, duplicate, weights, 2)), "Reject duplicate highlighted IDs");
    Require(FAILED(nb_set_highlight(renderer, unknown, weights, 1)), "Reject unknown highlighted ID");
    for (float invalid : {-0.1f, 1.1f, NAN})
        Require(FAILED(nb_set_highlight(renderer, ids, &invalid, 1)), "Reject invalid highlight weight");
    NbLine line{10, 20, 0.55f};
    Require(FAILED(nb_set_lines(renderer, &line, 513)) && FAILED(nb_set_lines(renderer, nullptr, 1)), "Reject invalid line bounds");
    NbLine invalidLine{10, 999, 1};
    Require(FAILED(nb_set_lines(renderer, &invalidLine, 1)), "Reject invalid line endpoint");
    invalidLine = {10, 20, NAN};
    Require(FAILED(nb_set_lines(renderer, &invalidLine, 1)), "Reject nonfinite line weight");
    Require(FAILED(nb_focus_set(renderer, unknown, 1)) && FAILED(nb_focus_set(renderer, nullptr, 1)), "Reject invalid focus set");
    NbView badView{};
    Require(FAILED(nb_get_view(renderer, &badView)), "Reject wrong view size");
    int picked = -1, tier = -1;
    Require(FAILED(nb_pick_lod(renderer, 0, 0, &picked, nullptr)) &&
            FAILED(nb_pick_lod(renderer, NAN, 0, &picked, &tier)), "Reject invalid LOD pick");

    auto baseline = View(renderer);
    Require(baseline.width == 960 && baseline.height == 720 &&
            baseline.scaleX == 1.5f && baseline.scaleY == 1.5f, "View must report drawable pixels and composition scales");
    Require(std::abs(baseline.pixelsPerUnitAtDistance1 - 360 / std::tan(24 * 3.14159265358979323846 / 180)) < 0.001,
            "View pixels-per-unit does not match the 48-degree camera");
    std::array<float, 3> photo{3, 0, 0};
    int normalWidth = ProjectedWidth(renderer, 20, photo);
    auto center = Project(baseline, photo);
    Hr(nb_pick_lod(renderer, center[0], center[1], &picked, &tier), "Pick photo and its current LOD");
    Require(picked == 20 && tier > 0, "Resolved photograph must report a nonzero LOD tier");
    shader_tests::VerifyViewPixel(baseline, photo);

    float sample = 0;
    Require(nebula_highlight::Ease(sample, 1, 0.18f, 1.0f / 60, false) && std::abs(sample - 0.18f) < 0.00001f,
            "Highlight current easing must be 0.18 per 60Hz frame");
    sample = 0;
    Require(nebula_highlight::Ease(sample, 1, 0.14f, 1.0f / 60, false) && std::abs(sample - 0.14f) < 0.00001f,
            "Highlight mix easing must be 0.14 per 60Hz frame");
    auto before = Stats(renderer).frames;
    Hr(nb_set_highlight(renderer, ids, weights, 2), "Apply weighted pinned highlight");
    ids[0] = 999; weights[0] = NAN;
    Idle(renderer);
    Require(Stats(renderer).frames > before + 4, "Highlight weights and mix did not tween");
    Require(ProjectedWidth(renderer, 20, photo) > normalWidth * 1.15f, "Highlighted photo did not receive the 18% lift");
    auto highlightedView = View(renderer);
    Require(memcmp(baseline.viewProj, highlightedView.viewProj, sizeof(baseline.viewProj)) == 0, "Highlight moved the camera");
    before = Stats(renderer).frames;
    Hr(nb_set_lines(renderer, &line, 1), "Draw selection constellation line");
    Wait(renderer, [before](const NbStats& s) { return s.frames > before; }, "Present constellation line");
    Idle(renderer);
    before = Stats(renderer).frames;
    Hr(nb_set_lines(renderer, nullptr, 0), "Clear constellation lines");
    Wait(renderer, [before](const NbStats& s) { return s.frames > before; }, "Present cleared lines");
    unsigned char mask[]{1, 0, 1};
    before = Stats(renderer).frames;
    Hr(nb_set_filter(renderer, mask, 3), "Filter highlighted photo structurally");
    Wait(renderer, [before](const NbStats& s) { return s.frames > before; }, "Apply highlight structural filter");
    Hr(nb_pick_lod(renderer, center[0], center[1], &picked, &tier), "Pick filtered highlight");
    Require(picked == -1 && tier == -1, "Highlight must not resurrect a filtered photo");
    mask[1] = 1;
    Hr(nb_set_filter(renderer, mask, 3), "Restore highlighted photo");
    Hr(nb_set_highlight(renderer, nullptr, nullptr, 0), "Clear highlight and restore appearance");
    Idle(renderer);
    Require(ProjectedWidth(renderer, 20, photo) == normalWidth, "Clear highlight did not restore identity geometry");

    auto orientation = View(renderer);
    std::array<float, 3> forward{orientation.viewProj[12], orientation.viewProj[13], orientation.viewProj[14]};
    NbItem overlap[]{
        {10, -forward[0], -forward[1], -forward[2], 1, 0.4f, 0.4f, 0.4f, path},
        {20, forward[0], forward[1], forward[2], 1, 0.6f, 0.6f, 0.6f, path}
    };
    Hr(nb_set_scene(renderer, overlap, 2), "Create overlapping highlight depth fixture");
    Wait(renderer, [](const NbStats& s) { return s.items == 2 && s.resident == 2 && !s.pending; }, "Load depth fixture");
    Idle(renderer);
    Hr(nb_pick_lod(renderer, 480, 360, &picked, &tier), "Pick physical front photo");
    Require(picked == 10, "Overlap fixture did not establish normal depth ordering");
    uint32_t onTop = 20;
    float fullWeight = 1;
    Hr(nb_set_highlight(renderer, &onTop, &fullWeight, 1), "Highlight rear photo without overriding physical depth");
    Idle(renderer);
    Hr(nb_pick_lod(renderer, 480, 360, &picked, &tier), "Pick highlighted presentation layer");
    // Presentation now reserves depth priority for gathered/overlay photos.
    Require(picked == 10, "Highlight incorrectly overrode physical foreground depth");
    Hr(nb_set_pixels(renderer, 2), "Compose highlight with Pixels mode");
    NbResult match{10, 0, 0, 0, 1};
    Hr(nb_set_results(renderer, &match, 1), "Compose highlight with search and receded nonmatches");
    Idle(renderer);
    Hr(nb_pick_lod(renderer, 480, 360, &picked, &tier), "Pick search result over receded highlighted context");
    Require(picked == 10, "Highlight resurrected a receded search nonmatch");
    Hr(nb_clear_results(renderer), "Restore highlight after clearing search");
    Idle(renderer);
    Hr(nb_pick_lod(renderer, 480, 360, &picked, &tier), "Pick restored highlighted layer");
    Require(picked == 10, "Clearing search did not restore physical foreground depth");
    Hr(nb_select(renderer, 10), "Compose selected photo with highlighted layer");
    Idle(renderer);
    Hr(nb_pick_lod(renderer, 480, 360, &picked, &tier), "Pick selection above highlight");
    Require(picked == 10, "Highlight layer occluded the selected photograph");
    Hr(nb_set_pixels(renderer, 0), "Restore photo rendering");
    Hr(nb_set_scene(renderer, scene, 3), "Restore focus-set fixture and cancel highlight generation");
    Wait(renderer, [](const NbStats& s) { return s.items == 3 && s.resident == 3 && !s.pending; }, "Restore focus fixture");
    Idle(renderer);

    Hr(nb_set_reduced_motion(renderer, true), "Snap focus-set fixture layouts");
    float positions[]{10, -2, 1, 14, 2, 3, 12, 4, 2};
    before = Stats(renderer).frames;
    Hr(nb_set_positions(renderer, positions, 3), "Update target positions for focus-set centroid");
    Wait(renderer, [before](const NbStats& s) { return s.frames > before; }, "Present focus-set target layout");
    Idle(renderer);
    auto beforeFocus = View(renderer);
    Hr(nb_set_reduced_motion(renderer, false), "Enable animated focus-set camera");
    uint32_t focus[]{10, 20};
    before = Stats(renderer).frames;
    Hr(nb_focus_set(renderer, focus, 2), "Frame target-position centroid");
    Wait(renderer, [&](const NbStats& s) {
        return s.frames > before && std::abs(View(renderer).viewProj[3] - beforeFocus.viewProj[3]) > 0.01f;
    }, "Observe the animated, not desired, focus-set view");
    auto movingCentroid = Project(View(renderer), {12, 0, 2});
    Require(std::abs(movingCentroid[0] - 480) > 1 || std::abs(movingCentroid[1] - 360) > 1 ||
            std::abs(movingCentroid[2] - 260.0f / 32) > 0.01f,
            "GetView returned the desired camera instead of the current animated frame");
    Idle(renderer);
    Require(Stats(renderer).frames > before + 4, "FocusSet did not use camera animation");
    auto focused = View(renderer);
    auto centroid = Project(focused, {12, 0, 2});
    Require(std::abs(centroid[0] - 480) < 0.01f && std::abs(centroid[1] - 360) < 0.01f &&
            std::abs(centroid[2] - 260.0f / 32) < 0.01f, "FocusSet centroid/minimum distance is wrong");
    for (int i : {0, 1, 2, 4, 5, 6, 12, 13, 14})
        Require(std::abs(focused.viewProj[i] - beforeFocus.viewProj[i]) < 0.00001f, "FocusSet changed orbit angles");
    shader_tests::VerifyViewPixel(focused, {14, 2, 3});
    Hr(nb_back(renderer), "Restore camera after focus set");
    Idle(renderer);
    auto back = View(renderer);
    for (int i = 0; i < 16; ++i)
        Require(std::abs(back.viewProj[i] - beforeFocus.viewProj[i]) < 0.00001f, "Back did not restore pre-FocusSet camera");
    Hr(nb_set_reduced_motion(renderer, true), "Snap maximum-distance fixture");
    float wide[]{-100, 0, 0, 100, 0, 0, 0, 0, 0};
    Hr(nb_set_positions(renderer, wide, 3), "Set wide focus span");
    Hr(nb_focus_set(renderer, focus, 2), "Clamp focus-set maximum distance");
    Idle(renderer);
    Require(std::abs(Project(View(renderer), {0, 0, 0})[2] - 100) < 0.001f, "FocusSet maximum must be 3200/32");
    before = Stats(renderer).frames;
    Hr(nb_focus_set(renderer, nullptr, 0), "Empty focus set is a no-op");
    for (int i = 0; i < 5; ++i) {
        (void)View(renderer);
        std::this_thread::sleep_for(20ms);
    }
    Require(Stats(renderer).frames == before, "FocusSet(empty)/GetView polling woke the renderer");

    NbItem farPhoto{50, 0, 0, 0, 1, 0.6f, 0.5f, 0.4f, path};
    Hr(nb_set_scene(renderer, &farPhoto, 1), "Reset highlight/line generation");
    Wait(renderer, [](const NbStats& s) { return s.items == 1; }, "Present far-pick fixture");
    Hr(nb_zoom(renderer, -12000), "Zoom to far-dot LOD");
    Idle(renderer);
    auto distant = Project(View(renderer), {0, 0, 0});
    Hr(nb_pick_lod(renderer, distant[0], distant[1], &picked, &tier), "Read far-dot LOD pick");
    Require(picked == 50 && tier == 0, "Far photos must report tier zero");
    Hr(nb_pick(renderer, distant[0], distant[1], &picked), "Legacy pick must agree with LOD pick");
    Require(picked == 50, "PickLod changed legacy picking semantics");
    Hr(nb_set_reduced_motion(renderer, false), "Restore motion after highlight tests");
    Idle(renderer);
    std::cout << "PASS highlight/view ABI: 12-byte lines, 88-byte view, weight tween/lift/clear, filters,\n"
              << "     animated FocusSet target-centroid/clamps/back, unchanged angles, query-only view, resolved/far PickLod\n";
}

double Percentile(std::vector<double> values, double percentile) {
    Require(!values.empty(), "No performance samples collected");
    std::sort(values.begin(), values.end());
    return values[static_cast<size_t>((values.size() - 1) * percentile)];
}

NbFocusStats FocusStatus(void* renderer) {
    NbFocusStats status{sizeof(NbFocusStats)};
    Hr(nb_get_focus_stats(renderer, &status), "Read focused-original status");
    Require(status.gpuBytes <= nebula_original::Budget, "Focused original exceeded its independent 96 MiB reserve");
    return status;
}

void OriginalTests(const wchar_t* thumbnail, const std::filesystem::path& output) {
    auto original = output / L"original-contract-3200.bmp";
    auto large = output / L"original-contract-roi.bmp";
    auto sharp = output / L"original-contract-sharp.bmp";
    report_tests::WriteOriginal(original, 3200, 2048);
    report_tests::WriteOriginal(sharp, 640, 384);
    report_tests::Orientations(output);
    depth_tests::Run(sharp);
    ComPtr<TestPanel> panel;
    panel.Attach(new TestPanel());
    Handle renderer;
    Hr(nb_create(static_cast<ISwapChainPanelNative*>(panel.Get()), &renderer.value), "Create original-path renderer");
    Hr(nb_resize(renderer.value, 960, 720, 1.5f, 1.5f), "Resize original-path renderer");
    Require(sizeof(NbItemV2) == 56 && sizeof(NbFocusStats) == 64, "Original-path ABI mismatch");
    Require(FAILED(nb_set_depth_options(renderer.value, 2, 1)), "Reject invalid depth option");
    NbItemV2 item{{77,0,0,0,3200.0f/2048,0.5f,0.5f,0.5f,thumbnail},original.c_str(),1};
    Hr(nb_set_scene_v2(renderer.value, &item, 1), "Copy atomic thumbnail/original scene");
    Wait(renderer.value, [](const NbStats& s) { return s.items == 1 && !s.pending && s.resident == 1; }, "Load original fixture thumbnail");
    auto waitFocus = [&](int id, uint32_t state) {
        auto deadline = std::chrono::steady_clock::now() + 20s;
        for (;;) {
            auto focus = FocusStatus(renderer.value);
            if (focus.state == 5) {
                std::cerr << "Original failure HRESULT 0x" << std::hex << static_cast<unsigned>(focus.error) << std::dec << '\n';
                throw std::runtime_error("Focused original decode failed");
            }
            if (focus.id == id && focus.state == state && !focus.pending) return focus;
            Require(std::chrono::steady_clock::now() < deadline, "Focused-original refinement did not complete");
            std::this_thread::sleep_for(10ms);
        }
    };
    auto start = std::chrono::steady_clock::now();
    Hr(nb_select(renderer.value, 77), "Prioritize clicked original without moving camera");
    auto full = waitFocus(77, 3);
    Require(full.sourceWidth == 3200 && full.textureWidth == 3200 && full.textureHeight == 2048,
            "Focused original remained capped at thumbnail or 2048 intermediate resolution");
    std::cout << "Original 3200px ready in " << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()
              << "ms; WIC decode=" << full.decodeMs << "ms\n";
    Idle(renderer.value);
    Hr(nb_set_depth_options(renderer.value, true, false), "Disable DoF independently");
    Idle(renderer.value);
    Hr(nb_set_depth_options(renderer.value, true, true), "Restore protected focus DoF");
    Hr(nb_set_reduced_motion(renderer.value, true), "Snap rack-focus under reduced motion");
    Idle(renderer.value);
    report_tests::WriteOriginal(large, 8192, 4096);
    auto pausedFrame = Stats(renderer.value).frames;
    Hr(nb_resize(renderer.value, 0, 0, 1.5f, 1.5f), "Hide surface before original decoding");
    std::this_thread::sleep_for(100ms);
    item.item.id = 88; item.item.aspect = 2; item.originalPath = large.c_str(); item.contentVersion = 2;
    Hr(nb_set_scene_v2(renderer.value, &item, 1), "Replace source generation while preserving legacy ABI");
    Hr(nb_select(renderer.value, 88), "Decode bounded intermediate for oversized original");
    std::this_thread::sleep_for(300ms);
    Require(Stats(renderer.value).frames == pausedFrame, "Hidden original/cache work presented a frame");
    Hr(nb_resize(renderer.value, 960, 720, 1.5f, 1.5f), "Resume original upload on visible surface");
    auto intermediate = waitFocus(88, 2);
    Require(intermediate.sourceWidth == 8192 && intermediate.textureWidth == 2048,
            "Oversized original must expose its true dimensions and bounded intermediate state");
    Hr(nb_zoom(renderer.value, 1800), "Request original-pixel ROI at inspection zoom");
    auto roi = waitFocus(88, 4);
    Require(roi.roiWidth == roi.textureWidth && roi.roiHeight == roi.textureHeight &&
            roi.textureWidth < roi.sourceWidth && roi.gpuBytes <= nebula_original::Budget,
            "Inspection ROI must preserve original pixels within the hard reserve");
    Idle(renderer.value);
    auto frame = Stats(renderer.value).frames;
    for (int i = 0; i < 100; ++i) {
        std::this_thread::sleep_for(100ms);
        Require(Stats(renderer.value).frames == frame && Stats(renderer.value).fps == 0, "Original/DoF kept rendering during ten-second settled idle");
    }
    Hr(nb_set_scene(renderer.value, nullptr, 0), "Cancel original source on legacy scene replacement");
    Wait(renderer.value, [](const NbStats& s) { return s.items == 0 && s.pending == 0; }, "Clear original generation");
    Require(FocusStatus(renderer.value).id == -1, "Old focused source survived scene replacement");
    std::filesystem::remove(original); std::filesystem::remove(large); std::filesystem::remove(sharp);
    std::cout << "PASS focused-original API: full 3200px refinement, 8192px source ROI, 96MiB reserve,\n"
              << "     generation cancellation, depth options, reduced motion and ten-second zero-frame idle\n";
}

void PhotoFailureTests(const wchar_t* thumbnail, const std::filesystem::path& output) {
    auto truncated = output / L"decode-truncated.bmp";
    WriteThumbnail(truncated);
    std::filesystem::resize_file(truncated, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + 16);
    ComPtr<IWICImagingFactory> factory;
    Hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "Create corrupt-file WIC factory");
    bool rejected = false;
    try { nebula_decode::Decode(factory.Get(), truncated.wstring(), 256, false); }
    catch (const nebula_original::Failure&) { rejected = true; }
    Require(rejected, "Truncated pixel data was incorrectly accepted by metadata fallback");
    ComPtr<TestPanel> panel;
    panel.Attach(new TestPanel());
    Handle renderer;
    Hr(nb_create(static_cast<ISwapChainPanelNative*>(panel.Get()), &renderer.value), "Create isolated photo-failure renderer");
    Hr(nb_resize(renderer.value, 960, 720, 1.5f, 1.5f), "Resize photo-failure renderer");
    NbItemV2 photos[]{
        {{41,-1,0,0,2,0.7f,0.3f,0.2f,thumbnail},truncated.c_str(),1},
        {{42,1,0,0,2,0.2f,0.6f,0.4f,truncated.c_str()},truncated.c_str(),2}
    };
    Hr(nb_set_scene_v2(renderer.value, photos, 2), "Decode corrupt files without setting renderer error");
    Wait(renderer.value, [](const NbStats& stats) { return stats.items == 2 && stats.failed >= 1 && stats.resident >= 1 && !stats.pending; },
         "Good lower-resolution photo disappeared after another tier failed");
    Hr(nb_select(renderer.value, 41), "Select photo with genuinely unreadable original");
    Wait(renderer.value, [&](const NbStats& stats) {
        auto focus = FocusStatus(renderer.value);
        return stats.failed == 2 && !stats.pending && focus.id == 41 && focus.state == 5 && FAILED(focus.error);
    }, "Unreadable original was not exposed as per-photo state");
    for (int i = 0; i < 3; ++i) {
        Hr(nb_select(renderer.value, -1), "Clear failed original selection");
        std::this_thread::sleep_for(20ms);
        Hr(nb_select(renderer.value, 41), "Reselect failed original without recounting");
        std::this_thread::sleep_for(20ms);
    }
    Idle(renderer.value);
    Require(Stats(renderer.value).failed == 2 && Lod(renderer.value).failed == 2 && Stats(renderer.value).resident >= 1,
            "Failures must count unique photos and preserve good image tiers");
    wchar_t error[2048]{};
    Hr(nb_get_error(renderer.value, error, ARRAYSIZE(error)), "Read renderer error after per-photo failures");
    Require(!error[0], "Per-photo failure leaked into GetError");
    for (int i = 0; i < 8; ++i) {
        Hr(nb_set_scene_v2(renderer.value, photos, 2), "Queue failing generation for cancellation");
        Hr(nb_select(renderer.value, 41), "Queue failing original for cancellation");
    }
    NbItemV2 good{{99,0,0,0,2,0.7f,0.3f,0.2f,thumbnail},thumbnail,3};
    Hr(nb_set_scene_v2(renderer.value, &good, 1), "Supersede failing decodes with valid generation");
    Wait(renderer.value, [](const NbStats& stats) { return stats.items == 1 && !stats.pending && stats.resident == 1; },
         "Cancelled failing generations did not drain");
    Idle(renderer.value);
    Require(Stats(renderer.value).failed == 0 && Lod(renderer.value).failed == 0, "Cancelled errors contaminated new scene counters");
    Hr(nb_get_error(renderer.value, error, ARRAYSIZE(error)), "Read renderer error after cancellation");
    Require(!error[0], "Cancelled decode changed GetError");
    std::filesystem::remove(truncated);
    std::cout << "PASS per-photo failures: truncated pixels rejected, healthy lower tiers retained,\n"
              << "     unique failure counters, no GetError, cancellation and zero idle frames\n";
}

void NearFadeTests(const wchar_t* path) {
    using namespace DirectX;
    ComPtr<TestPanel> panel; panel.Attach(new TestPanel()); Handle renderer;
    Hr(nb_create(static_cast<ISwapChainPanelNative*>(panel.Get()),&renderer.value),"Create near-fade renderer");
    Hr(nb_resize(renderer.value,1280,900,1.5f,1.5f),"Resize near-fade renderer");
    NbItem items[]{{0,-10,0,0,1,.5f,.5f,.5f,path},{1,10,0,0,1,.5f,.5f,.5f,path},
                   {2,0,-1,0,1,.5f,.5f,.5f,path},{3,0,1,0,1,.5f,.5f,.5f,path}};
    Hr(nb_set_scene(renderer.value,items,4),"Create symmetric fixed-camera fade scene");
    Idle(renderer.value);
    auto snapshot=View(renderer.value);
    XMFLOAT4X4 stored; memcpy(&stored,snapshot.viewProj,sizeof(stored));
    XMMATRIX projection=XMMatrixPerspectiveFovLH(48*XM_PI/180,snapshot.width/snapshot.height,10000000,.001f);
    XMMATRIX view=XMMatrixTranspose(XMLoadFloat4x4(&stored))*XMMatrixInverse(nullptr,projection);
    XMMATRIX inverse=XMMatrixInverse(nullptr,view);
    XMVECTOR eye=inverse.r[3],forward=inverse.r[2];
    float distance=XMVectorGetX(XMVector3Length(eye));
    XMFLOAT3 positions[]{{-10,0,0},{10,0,0},{},{}};
    Require(distance>10,"Near-fade scene must frame from more than ten tile widths");
    XMStoreFloat3(&positions[2],eye+forward*4.0f);
    XMStoreFloat3(&positions[3],eye+forward*(.5f*distance));
    Hr(nb_set_positions(renderer.value,reinterpret_cast<const float*>(positions),4),"Place probes at 4 tiles and .5D");
    Idle(renderer.value);
    int id=-1;
    Hr(nb_pick(renderer.value,640,450,&id),"Pick near probes");
    Require(id==2,"A normal-size photo in front of the target was faded (orbit-relative fade is gone)");
    XMStoreFloat3(&positions[2],eye+forward*.05f);
    Hr(nb_set_positions(renderer.value,reinterpret_cast<const float*>(positions),4),"Move probe onto the near plane");
    Idle(renderer.value);
    Hr(nb_pick(renderer.value,640,450,&id),"Pick near-plane probe");
    Require(id==3,"The photo at the near plane remained pickable in front of the visible .5D photo");
    unsigned char nearOnly[]{0,0,1,0};
    Hr(nb_set_filter(renderer.value,nearOnly,4),"Isolate fully faded near photo");
    Idle(renderer.value);
    Hr(nb_pick(renderer.value,640,450,&id),"Pick fully faded near photo");
    Require(id==-1 && Lod(renderer.value).visible==0,"Fully faded photo was drawn or pickable");
    unsigned char farOnly[]{0,0,0,1};
    Hr(nb_set_filter(renderer.value,farOnly,4),"Restore visible .5D photo");
    Idle(renderer.value);
    Hr(nb_pick(renderer.value,640,450,&id),"Pick fully visible .5D photo");
    Require(id==3,"The .5D photo was incorrectly culled");
    XMStoreFloat3(&positions[3],eye+forward*.5f);
    Hr(nb_set_positions(renderer.value,reinterpret_cast<const float*>(positions),4),"Push unselected photo beyond viewport-size safety");
    Idle(renderer.value);
    Hr(nb_pick(renderer.value,640,450,&id),"Pick oversized unselected photo");
    Require(id==-1 && Lod(renderer.value).visible==0,"Oversized unselected photo remained visible or pickable");
    Hr(nb_select(renderer.value,3),"Exempt intentional selected inspection from safety fade");
    Idle(renderer.value);
    Hr(nb_pick(renderer.value,640,450,&id),"Pick oversized selected photo");
    Require(id==3 && Lod(renderer.value).visible==1,"Selected inspection lost its near/size exemption");
    std::cout<<"PASS native near fade: 4-tile photo visible in front, near-plane photo hidden; oversized hidden except selection\n";
}

void Stress100K(const wchar_t* path, const std::filesystem::path& outputDirectory) {
    ComPtr<TestPanel> panel;
    panel.Attach(new TestPanel());
    Handle renderer;
    Hr(nb_create(static_cast<ISwapChainPanelNative*>(panel.Get()), &renderer.value), "Create 100K renderer");
    constexpr int drawableWidth = 3274, drawableHeight = 2190;
    Hr(nb_resize(renderer.value, drawableWidth, drawableHeight, 2.25f, 2.25f), "Resize report-matched 100K surface");
    std::vector<NbItem> items;
    items.reserve(100000);
    std::mt19937 random(83457);
    std::uniform_real_distribution<float> uniform(-1, 1);
    float radius = 1.1f * std::cbrt(100000.0f);
    uint32_t central = 0;
    float nearest = FLT_MAX;
    for (uint32_t i = 0; i < 100000; ++i) {
        float x, y, z;
        do { x = uniform(random); y = uniform(random); z = uniform(random); } while (x*x + y*y + z*z > 1);
        x *= radius * 1.4f; y *= radius; z *= radius * 0.8f;
        float distance = x * x + y * y + z * z;
        if (distance < nearest) { nearest = distance; central = i; }
        items.push_back({i, x, y, z, 2, 0.4f + (i % 5) * 0.1f, 0.45f, 0.35f, path});
    }
    MemoryBudgetProbe memory(panel->chain.Get(), nebula_limits::HundredThousandMemoryBudget);
    auto loadStart = std::chrono::steady_clock::now();
    Hr(nb_set_scene(renderer.value, items.data(), static_cast<int>(items.size())), "Set synthetic 100000-photo scene");
    auto deadline = loadStart + 240s;
    uint32_t nextProgress = 25000;
    for (;;) {
        auto lod = Lod(renderer.value);
        memory.Sample();
        if (lod.mosaicReady >= nextProgress) {
            std::cout << "100K resident mosaic progress: " << lod.mosaicReady << "/100000\n";
            nextProgress += 25000;
        }
        if (lod.items == 100000 && lod.mosaicReady == 100000 && lod.pending == 0) break;
        Require(std::chrono::steady_clock::now() < deadline, "100K progressive mosaic load did not finish within 240 seconds");
        std::this_thread::sleep_for(10ms);
    }
    Idle(renderer.value);
    double loadSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - loadStart).count();
    auto fitted = Lod(renderer.value);
    Require(fitted.dots + fitted.mosaics > fitted.visible * 0.95f, "100K fit should use density/mosaics rather than full image detail");
    std::ofstream report(outputDirectory / L"stress-100k.tsv");
    Require(report.good(), "Open persistent 100K performance report");
    report << "pose\tframes\tcpu_p50_ms\tcpu_p95_ms\tgpu_p50_ms\tgpu_p95_ms\tcpu_plus_gpu_p50_ms\tcpu_plus_gpu_p95_ms"
              "\tframe_p50_ms\tframe_p95_ms\tpick_p50_ms\tpick_p95_ms\tflips_per_sec\tevictions_per_sec"
              "\tdots\tmosaics\tthumbnails\tpreviews\tthumb_resident\tpreview_resident\tmemory_bytes\n";
    auto measure = [&](const char* pose) {
        std::map<uint64_t, double> cpuByFrame, gpuByFrame;
        std::vector<double> cpu, gpu, total, frameTime, pickTime;
        std::vector<double> preparation, ordering, uploading, submission;
        auto baseline = Lod(renderer.value);
        auto start = std::chrono::steady_clock::now();
        auto nextInput = start;
        uint64_t lastFrame = baseline.frames, lastGpu = baseline.gpuFrame;
        uint32_t tick = 0;
        auto end = start + 8s;
        while (std::chrono::steady_clock::now() < end) {
            auto now = std::chrono::steady_clock::now();
            if (now >= nextInput) {
                Hr(nb_orbit(renderer.value, 0.75f, std::sin(tick * 0.035f) * 0.2f), "100K orbit input");
                if (tick % 8 == 0) Hr(nb_zoom(renderer.value, std::sin(tick * 0.05f) * 6), "100K continuous zoom input");
                nextInput = now + 16ms;
                ++tick;
            }
            auto lod = Lod(renderer.value);
            memory.Sample();
            Require(lod.dots + lod.mosaics + lod.thumbnails + lod.previews == lod.visible, "LOD histogram does not cover visible items");
            if (lod.frames != lastFrame) {
                NbFrameDiagnostics diagnostics{sizeof(NbFrameDiagnostics)};
                Hr(nb_get_frame_diagnostics(renderer.value, &diagnostics), "Read CPU pass diagnostics");
                Require(diagnostics.tiles + diagnostics.dots <= lod.items, "Submitted draw counters overflowed scene size");
                preparation.push_back(diagnostics.prepareMs); ordering.push_back(diagnostics.orderMs);
                uploading.push_back(diagnostics.uploadMs); submission.push_back(diagnostics.submitMs);
                cpu.push_back(lod.cpuMs);
                frameTime.push_back(lod.frameMs);
                cpuByFrame[lod.frames] = lod.cpuMs;
                lastFrame = lod.frames;
                for (int sample = 0; sample < 8; ++sample) {
                    float x = drawableWidth * 0.25f + static_cast<float>(random() % (drawableWidth / 2));
                    float y = drawableHeight * 0.25f + static_cast<float>(random() % (drawableHeight / 2));
                    int id, tier;
                    auto pickStart = std::chrono::steady_clock::now();
                    Hr(nb_pick_lod(renderer.value, x, y, &id, &tier), "100K hierarchical-grid pick");
                    pickTime.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pickStart).count());
                }
            }
            if (lod.gpuFrame != lastGpu && lod.gpuFrame > baseline.frames) {
                gpu.push_back(lod.gpuMs);
                gpuByFrame[lod.gpuFrame] = lod.gpuMs;
                lastGpu = lod.gpuFrame;
            }
            std::this_thread::sleep_for(2ms);
        }
        auto final = Lod(renderer.value);
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        for (const auto& [frame, value] : gpuByFrame)
            if (auto found = cpuByFrame.find(frame); found != cpuByFrame.end()) total.push_back(found->second + value);
        Require(cpu.size() >= 20 && gpu.size() >= 15 && total.size() >= 10, "Insufficient 100K CPU/GPU timing samples");
        double pick95 = Percentile(pickTime, 0.95);
        Require(pick95 < 1, "100K picking p95 exceeded 1 ms");
        double flips = (final.flips - baseline.flips) / seconds;
        double evictions = (final.evictions - baseline.evictions) / seconds;
        Require(flips < 50000, "Slow 100K orbit caused pathological LOD tier thrashing");
        std::cout << std::fixed << std::setprecision(3)
                  << "100K " << pose << ": frames=" << cpu.size() << " CPU p50/p95=" << Percentile(cpu, .5) << "/" << Percentile(cpu, .95)
                  << " ms GPU=" << Percentile(gpu, .5) << "/" << Percentile(gpu, .95)
                  << " ms CPU+GPU=" << Percentile(total, .5) << "/" << Percentile(total, .95)
                  << " ms pick=" << Percentile(pickTime, .5) << "/" << pick95
                  << " ms flips/s=" << flips << " evictions/s=" << evictions
                  << " tiers=" << final.dots << "," << final.mosaics << "," << final.thumbnails << "," << final.previews
                  << " pools=" << final.thumbnailResident << "/" << final.previewResident << '\n';
        std::cout << "CPU stages p50/p95: prepare=" << Percentile(preparation,.5) << "/" << Percentile(preparation,.95)
                  << " order=" << Percentile(ordering,.5) << "/" << Percentile(ordering,.95)
                  << " upload=" << Percentile(uploading,.5) << "/" << Percentile(uploading,.95)
                  << " submit=" << Percentile(submission,.5) << "/" << Percentile(submission,.95) << '\n';
#ifndef _DEBUG
        if (std::string_view(pose) == "fit")
            Require(Percentile(cpu, 0.95) <= 8, "Report P1 acceptance failed: 100K fit CPU p95 exceeds 8 ms");
#endif
        report << pose << '\t' << cpu.size() << '\t' << Percentile(cpu, .5) << '\t' << Percentile(cpu, .95)
               << '\t' << Percentile(gpu, .5) << '\t' << Percentile(gpu, .95) << '\t' << Percentile(total, .5) << '\t' << Percentile(total, .95)
               << '\t' << Percentile(frameTime, .5) << '\t' << Percentile(frameTime, .95)
               << '\t' << Percentile(pickTime, .5) << '\t' << pick95 << '\t' << flips << '\t' << evictions
               << '\t' << final.dots << '\t' << final.mosaics << '\t' << final.thumbnails << '\t' << final.previews
               << '\t' << final.thumbnailResident << '\t' << final.previewResident << '\t' << memory.Peak() << '\n';
    };
    measure("fit");
    Hr(nb_focus(renderer.value, static_cast<int>(central)), "Focus real central photo for close 100K view");
    Idle(renderer.value);
    Hr(nb_select(renderer.value, -1), "Measure close LOD without selection-isolation overriding nearby photos");
    Idle(renderer.value);
    auto close = Lod(renderer.value);
    Require(close.previews > 0 && close.previewResident > 0, "Close view did not request and display high-resolution mip previews");
    measure("close");
    auto beforeIdle = Lod(renderer.value);
    try { Idle(renderer.value); }
    catch (...) {
        auto failedIdle = Lod(renderer.value);
        NbFrameDiagnostics state{sizeof(NbFrameDiagnostics)};
        nb_get_frame_diagnostics(renderer.value, &state);
        std::cerr << "Idle diagnostics: uploads+=" << failedIdle.uploads - beforeIdle.uploads
                  << " evictions+=" << failedIdle.evictions - beforeIdle.evictions
                  << " flips+=" << failedIdle.flips - beforeIdle.flips
                  << " pools=" << failedIdle.thumbnailResident << "/" << failedIdle.previewResident
                  << " caps=" << failedIdle.cap0 << "," << failedIdle.cap1 << "," << failedIdle.cap2
                  << " movingFlags=" << state.reserved << '\n';
        throw;
    }
    memory.Report(100000);
    report << "# mosaic_load_seconds\t" << loadSeconds << "\n# synthetic_100000_ids_share_one_generated_256px_BMP; not distinct-Met IO throughput\n";
    Require(report.good(), "Write 100K performance report");
    std::cout << "PASS 100K scale: all 1364-byte mosaics resident, bounded 64/128/256/512 mip pools, grid picks, GPU timing,\n"
              << "     bounded flips, <=1.2GB conservative CPU+GPU memory, and zero idle presents; load=" << loadSeconds << "s\n";
}
struct LibraryPhoto {
    std::wstring thumbnail, original;
    float aspect, red, green, blue;
    uint32_t width, height;
};

std::vector<LibraryPhoto> ReadLibrary(const std::filesystem::path& path) {
    using namespace ABI::Windows::Data::Json;
    using Microsoft::WRL::Wrappers::HString;
    using Microsoft::WRL::Wrappers::HStringReference;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    Require(file.good(), "Open real library.json");
    auto length = file.tellg();
    Require(length > 0 && length <= 64 * 1024 * 1024, "Real-library JSON exceeds test input limit");
    std::string bytes(static_cast<size_t>(length), '\0');
    file.seekg(0);
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    Require(file.good(), "Read real library.json");
    int characters = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    Require(characters > 0, "Real library.json must contain valid UTF-8");
    std::wstring text(characters, L'\0');
    Require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), text.data(), characters) == characters,
            "Convert real-library JSON");
    ComPtr<IJsonObjectStatics> parser;
    Hr(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Data_Json_JsonObject).Get(), IID_PPV_ARGS(&parser)), "Activate SDK JSON parser");
    HString input;
    Hr(input.Set(text.data(), static_cast<UINT32>(text.size())), "Create JSON string");
    ComPtr<IJsonObject> library;
    Hr(parser->Parse(input.Get(), &library), "Parse real library.json");
    ComPtr<IJsonArray> photos;
    Hr(library->GetNamedArray(HStringReference(L"Photos").Get(), &photos), "Read Photos array");
    ComPtr<ABI::Windows::Foundation::Collections::IVector<IJsonValue*>> array;
    Hr(photos.As(&array), "Read Photos collection");
    UINT32 count = 0;
    Hr(array->get_Size(&count), "Read real-library photo count");
    Require(count >= 2 && count <= nebula_limits::MaxItems, "Real-library regression needs 2..100000 distinct photos");
    std::vector<LibraryPhoto> result;
    result.reserve(count);
    std::unordered_set<std::wstring> distinct;
    for (UINT32 i = 0; i < count; ++i) {
        ComPtr<IJsonObject> photo;
        Hr(photos->GetObjectAt(i, &photo), "Read photo metadata");
        auto string = [&](const wchar_t* name) {
            HString value;
            Hr(photo->GetNamedString(HStringReference(name, static_cast<UINT32>(wcslen(name))).Get(), value.GetAddressOf()), "Read photo source path");
            UINT32 size = 0;
            auto* data = value.GetRawBuffer(&size);
            return std::wstring(data, size);
        };
        auto number = [&](const wchar_t* name) {
            double value = 0;
            Hr(photo->GetNamedNumber(HStringReference(name, static_cast<UINT32>(wcslen(name))).Get(), &value), "Read photo numeric metadata");
            Require(std::isfinite(value), "Non-finite real-library metadata");
            return value;
        };
        LibraryPhoto row{string(L"ThumbnailPath"), string(L"OriginalPath"),
            static_cast<float>(number(L"Aspect")), static_cast<float>(number(L"Red")),
            static_cast<float>(number(L"Green")), static_cast<float>(number(L"Blue")), 0, 0};
        double width = number(L"Width"), height = number(L"Height");
        Require(width >= 1 && height >= 1 && width <= 100000 && height <= 100000, "Invalid real-library image dimensions");
        row.width = static_cast<uint32_t>(width); row.height = static_cast<uint32_t>(height);
        Require(std::filesystem::is_regular_file(row.thumbnail) && std::filesystem::is_regular_file(row.original), "Real-library source missing");
        Require(distinct.insert(row.thumbnail).second, "Real-library regression requires distinct thumbnails, not a shared fixture");
        result.push_back(std::move(row));
    }
    return result;
}

void DecodeAudit(const std::filesystem::path& path, const std::filesystem::path& output) {
    auto photos = ReadLibrary(path);
    ComPtr<IWICImagingFactory> factory;
    Hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "Create audit WIC factory");
    std::ofstream report(output / L"decode-audit.tsv");
    Require(report.good(), "Open decode audit report");
    report << "id\tstatus\tstage\thresult\toperation\tpath\n";
    size_t failures = 0, recovered = 0, fullCount = 0, roiCount = 0, mosaicCount = 0, cached = 0;
    std::array<size_t, 4> thumbnailCounts{};
    size_t previewCount = 0, intermediateCount = 0;
    auto cacheRoot = output / (L"decode-audit-cache-" + std::to_wstring(GetCurrentProcessId()));
    auto writeText = [&](const std::wstring& value) {
        auto bytes = std::filesystem::path(value).u8string();
        report.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    };
    auto start = std::chrono::steady_clock::now();
    {
    nebula_cache::Store cache(0xdec0de, cacheRoot);
    for (size_t i = 0; i < photos.size(); ++i) {
        const auto& photo = photos[i];
        std::string stage;
        const std::wstring* sourcePath = &photo.thumbnail;
        bool metadataReported = false;
        auto record = [&](const char* status, HRESULT hr, const wchar_t* operation) {
            report << i << '\t' << status << '\t' << stage << "\t0x" << std::hex << static_cast<unsigned>(hr) << std::dec << '\t';
            writeText(operation); report << '\t'; writeText(*sourcePath); report << '\n';
            report.flush();
        };
        auto metadata = [&](const nebula_original::MetadataWarning& warning) {
            if (FAILED(warning.hr) && !metadataReported) {
                ++recovered; metadataReported = true;
                record("metadata-fallback", warning.hr, warning.operation);
            }
        };
        try {
            constexpr UINT edges[]{64,128,224,256};
            for (size_t tier = 0; tier < std::size(edges); ++tier) {
                UINT edge = edges[tier];
                stage = "thumbnail-" + std::to_string(edge);
                auto image = nebula_decode::Decode(factory.Get(), photo.thumbnail, edge, false);
                metadata(image.metadata);
                Require(image.pixels.size() == static_cast<size_t>(edge) * edge * 4, "Audit thumbnail output shape mismatch");
                ++thumbnailCounts[tier];
                if (edge == 64) {
                    stage = "bc7-cache-64";
                    auto encoded = nebula_cache::Encode(image.pixels, edge, image.aspect);
                    auto key = nebula_cache::SourceKey(photo.thumbnail, i + 1);
                    nebula_original::Check(cache.Write(key, encoded), L"Write audit BC7 chain");
                    nebula_cache::Payload read;
                    nebula_original::Check(cache.Read(key, edge, read), L"Read mapped audit BC7 chain");
                    Require(encoded.bytes == read.bytes && encoded.offsets == read.offsets, "Real-photo BC7 cache roundtrip mismatch");
                    ++cached;
                }
                if (edge == 256) {
                    auto mosaic = BuildMosaicAverages(image.pixels, edge, image.x, image.y, image.width, image.height, image.aspect);
                    Require(mosaic.size() == nebula_limits::MosaicBytesPerSlice, "Real-photo mosaic byte count mismatch");
                    ++mosaicCount;
                }
            }
            sourcePath = &photo.original;
            stage = "original-512";
            auto preview = nebula_decode::Decode(factory.Get(), photo.original, 512, true);
            metadata(preview.metadata); ++previewCount;
            Require(preview.pixels.size() == 512 * 512 * 4, "Original preview output shape mismatch");
            ComPtr<IWICBitmapDecoder> decoder;
            ComPtr<IWICBitmapFrameDecode> frame;
            stage = "original-open";
            nebula_original::Check(factory->CreateDecoderFromFilename(photo.original.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder), L"Open audit original");
            nebula_original::Check(decoder->GetFrame(0, &frame), L"Read audit original frame");
            nebula_original::Request request{0,0,static_cast<int>(i),photo.original};
            stage = "focus-intermediate";
            auto intermediate = nebula_original::FramePixels(factory.Get(), frame.Get(), request, nullptr, 2048);
            metadata(intermediate.metadata); ++intermediateCount;
            UINT width = intermediate.sourceWidth, height = intermediate.sourceHeight;
            if (width <= 16384 && height <= 16384 && static_cast<uint64_t>(width) * height * 4 <= nebula_original::Budget) {
                stage = "focus-full";
                auto full = nebula_original::FramePixels(factory.Get(), frame.Get(), request, nullptr, 0);
                metadata(full.metadata); ++fullCount;
                Require(full.state == 3 && full.width == width && full.height == height, "Original full refinement is not source resolution");
            }
            stage = "focus-roi";
            UINT w = std::min(width, 1024u), h = std::min(height, 1024u);
            WICRect roi{static_cast<INT>((width - w) / 2), static_cast<INT>((height - h) / 2), static_cast<INT>(w), static_cast<INT>(h)};
            auto clipped = nebula_original::FramePixels(factory.Get(), frame.Get(), request, &roi, 0);
            metadata(clipped.metadata); ++roiCount;
            Require(clipped.state == 4 && clipped.width == w && clipped.height == h, "Original ROI did not retain exact source pixels");
        } catch (const nebula_original::Failure& error) {
            ++failures;
            record("failed", error.hr, error.operation);
            std::wcerr << L"Decode audit id=" << i << L" " << error.operation << L" HRESULT=0x" << std::hex
                       << static_cast<unsigned>(error.hr) << std::dec << L" path=" << *sourcePath << std::endl;
        } catch (const nebula_cache::Failure& error) {
            ++failures;
            record("failed", error.hr, L"Audit source hash");
        }
        if ((i + 1) % 500 == 0) std::cout << "Decode audit " << i + 1 << "/" << photos.size() << std::endl;
    }
    }
    std::filesystem::remove(cacheRoot / L"0000000000dec0de.pack");
    std::filesystem::remove(cacheRoot / L"0000000000dec0de.journal");
    std::filesystem::remove(cacheRoot / L"0000000000dec0de.manifest");
    std::filesystem::remove(cacheRoot);
    Require(failures == 0, "Real-library decode audit failed; see decode-audit.tsv");
    for (size_t count : thumbnailCounts) Require(count == photos.size(), "Audit skipped a thumbnail tier");
    Require(previewCount == photos.size() && intermediateCount == photos.size() && roiCount == photos.size() &&
            mosaicCount == photos.size() && cached == photos.size(), "Audit skipped an original/mosaic/cache path");
    report << "# metadata_recovered\t" << recovered << "\n# full_originals\t" << fullCount
           << "\n# photos_each_tier\t" << photos.size() << '\n';
    std::cout << "PASS decode audit: " << photos.size() << " each at thumbnail 64/128/224/256, mosaic, BC7 cache roundtrip,\n"
              << "     original 512/intermediate/ROI; " << fullCount << " full originals; " << recovered << " metadata fallbacks; zero failures; "
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() << "s" << std::endl;
}

void RealLibraryTests(const std::filesystem::path& path) {
    auto photos = ReadLibrary(path);
    std::cout << "Real-library regression: " << photos.size() << " distinct thumbnails and originals from "
              << path.filename().string() << std::endl;
    ComPtr<TestPanel> panel;
    panel.Attach(new TestPanel());
    Handle renderer;
    Hr(nb_create(static_cast<ISwapChainPanelNative*>(panel.Get()), &renderer.value), "Create real-library renderer");
    Hr(nb_resize(renderer.value, 1280, 900, 1.5f, 1.5f), "Resize real-library surface");
    std::vector<NbItemV2> scene;
    std::vector<float> positions;
    scene.reserve(photos.size()); positions.reserve(photos.size() * 3);
    for (size_t i = 0; i < photos.size(); ++i) {
        auto& photo = photos[i];
        float z = 1 - 2 * (static_cast<float>(i) + 0.5f) / static_cast<float>(photos.size());
        float angle = static_cast<float>(i) * 2.39996323f;
        float radius = 18 * std::sqrt(1 - z * z);
        float x = std::cos(angle) * radius, y = std::sin(angle) * radius;
        z *= 18;
        scene.push_back({{static_cast<uint32_t>(i),x,y,z,photo.aspect,photo.red,photo.green,photo.blue,photo.thumbnail.c_str()},
                         photo.original.c_str(),static_cast<uint64_t>(i + 1)});
        positions.insert(positions.end(), {x * 1.05f, y * 0.95f, z});
    }
    auto start = std::chrono::steady_clock::now();
    Hr(nb_set_scene_v2(renderer.value, scene.data(), static_cast<int>(scene.size())), "Set real scene-v2");
    std::this_thread::sleep_for(20ms);
    Hr(nb_set_positions(renderer.value, positions.data(), static_cast<int>(scene.size())), "Animate real layout during decoding");
    NbGlassPanel glass[]{{-24,18,260,680,18,0.55f,1},{290,18,950,72,18,0.55f,0.5f}};
    Hr(nb_set_glass_panels(renderer.value, glass, 2), "Composite glass during real-image streaming");
    uint32_t ids[]{0,static_cast<uint32_t>(photos.size() / 2)};
    float weights[]{1,0.6f};
    Hr(nb_set_highlight(renderer.value, ids, weights, 2), "Highlight during real-image streaming");
    size_t count = photos.size();
    auto loaded = [&] {
        auto deadline = std::chrono::steady_clock::now() + 240s;
        uint32_t next = 1000;
        for (;;) {
            auto stats = Stats(renderer.value);
            auto lod = Lod(renderer.value);
            Require(stats.failed == 0, "Real normalized thumbnails failed to decode");
            if (lod.mosaicReady >= next) {
                std::cout << "Real-library mosaics " << lod.mosaicReady << "/" << count << std::endl;
                next += 1000;
            }
            if (stats.items == count && lod.mosaicReady == count && !stats.pending) return;
            Require(std::chrono::steady_clock::now() < deadline, "Distinct real-library streaming did not drain");
            std::this_thread::sleep_for(10ms);
        }
    };
    loaded();
    Hr(nb_set_highlight(renderer.value, nullptr, nullptr, 0), "Clear real-library highlight");
    Idle(renderer.value);
    if (photos.size() >= 5000) {
        auto fit = Lod(renderer.value);
        Require(fit.visible && fit.dots + fit.mosaics >= fit.visible * 0.95f,
                "Real-library fit must be predominantly dots/mosaics, not tiny readable photos");
                        Require(fit.cap0 >= 10 && fit.cap1 >= 32 && fit.cap2 >= 110, "Real-library cap floors changed");
        std::cout << "PASS real-library fit LOD: dots=" << fit.dots << " mosaics=" << fit.mosaics
                  << " thumbnails=" << fit.thumbnails << " previews=" << fit.previews
                  << " caps=" << fit.cap0 << "/" << fit.cap1 << "/" << fit.cap2 << std::endl;
    }
    std::cout << "Real-library load " << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() << "s" << std::endl;
    size_t largest = 0;
    for (size_t i = 1; i < photos.size(); ++i)
        if (std::max(photos[i].width, photos[i].height) > std::max(photos[largest].width, photos[largest].height)) largest = i;
    for (size_t index : {size_t{0}, photos.size() / 2, largest}) {
        Hr(nb_select(renderer.value, static_cast<int>(index)), "Select distinct real original");
        Hr(nb_focus(renderer.value, static_cast<int>(index)), "Focus distinct real original");
        auto deadline = std::chrono::steady_clock::now() + 30s;
        for (;;) {
            auto stats = Stats(renderer.value);
            auto focus = FocusStatus(renderer.value);
            Require(focus.state != 5 && stats.failed == 0, "Real selected original failed");
            if (focus.id == static_cast<int>(index) && focus.state >= 2 && focus.state <= 4 && !focus.pending) {
                Require(focus.sourceWidth == photos[index].width && focus.sourceHeight == photos[index].height,
                        "Focused real original dimensions differ from importer metadata");
                Require(focus.textureWidth > 256 || focus.textureHeight > 256, "Real focused original remained thumbnail-only");
                std::cout << "Real original " << index << ": source=" << focus.sourceWidth << "x" << focus.sourceHeight
                          << " texture=" << focus.textureWidth << "x" << focus.textureHeight << " state=" << focus.state << std::endl;
                break;
            }
            Require(std::chrono::steady_clock::now() < deadline, "Real original refinement timed out");
            std::this_thread::sleep_for(10ms);
        }
    }
    // Supersede both original decoding and the ordinary WIC queues, then drain a new generation.
    for (int i = 0; i < 12; ++i) {
        Hr(nb_select(renderer.value, i % static_cast<int>(count)), "Cancel rapidly changing real selection");
        std::this_thread::sleep_for(4ms);
    }
    auto replacementFrame = Stats(renderer.value).frames;
    Hr(nb_set_scene_v2(renderer.value, scene.data(), static_cast<int>(scene.size())), "Replace real scene generation while decoding");
    Wait(renderer.value, [replacementFrame](const NbStats& stats) { return stats.frames >= replacementFrame + 2; }, "Present replaced real generation");
    loaded();
    Hr(nb_set_glass_panels(renderer.value, nullptr, 0), "Release real-library glass");
    Idle(renderer.value);
    auto frame = Stats(renderer.value).frames;
    std::this_thread::sleep_for(1s);
    Require(Stats(renderer.value).frames == frame && Stats(renderer.value).fps == 0, "Real-library settled scene presented idle frames");
    wchar_t error[2048]{};
    Hr(nb_get_error(renderer.value, error, ARRAYSIZE(error)), "Read real-library operational errors");
    if (error[0]) std::wcerr << error << '\n';
    Require(!error[0], "Real-library test retained a cache/decode/render error");
    std::cout << "PASS real-library scene-v2: all " << count << " distinct thumbnails, original refinement, layout/glass/highlight,\n"
              << "     focus/selection cancellation, scene replacement and zero-frame idle" << std::endl;
}
}

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 1;
    std::filesystem::path thumbnail;
    try {
        bool realOnly = argc == 4 && wcscmp(argv[2], L"--real-library-only") == 0;
        bool withLibrary = argc == 4 && wcscmp(argv[2], L"--library") == 0;
        bool auditOnly = argc == 4 && wcscmp(argv[2], L"--decode-audit-only") == 0;
        bool footprintOnly = argc == 3 && wcscmp(argv[2], L"--footprint-only") == 0;
        Require(argc == 2 || (argc == 3 && wcscmp(argv[2], L"--stress-only") == 0) || realOnly || withLibrary || auditOnly || footprintOnly,
                "Pass an output directory and optional --library/--real-library-only library.json");
        if (footprintOnly) {
            lod_tests::MotionReadback(argv[1]);
            CoUninitialize();
            return 0;
        }
        if (auditOnly) {
            DecodeAudit(argv[3], argv[1]);
            CoUninitialize();
            return 0;
        }
        if (realOnly) {
            RealLibraryTests(argv[3]);
            CoUninitialize();
            return 0;
        }
        if (argc == 3) {
            thumbnail = std::filesystem::path(argv[1]) / L"nebula-native-smoke-\u00e9\u65e5.bmp";
            WriteThumbnail(thumbnail);
            Stress100K(thumbnail.c_str(), std::filesystem::path(argv[1]));
            std::filesystem::remove(thumbnail);
            CoUninitialize();
            return 0;
        }
        CompileShaders();
        MosaicAverageTests();
        shader_tests::Run();
        thumbnail = std::filesystem::path(argv[1]) / L"nebula-native-smoke-\u00e9\u65e5.bmp";
        WriteThumbnail(thumbnail);
        auto path = thumbnail.wstring();
        void* invalid = reinterpret_cast<void*>(1);
        Require(FAILED(nb_create(nullptr, &invalid)) && invalid == nullptr, "Null panel must fail and clear output");
        auto* panel = new TestPanel();
        ComPtr<TestPanel> owner;
        owner.Attach(panel);
        {
            Handle renderer;
            Hr(nb_create(static_cast<ISwapChainPanelNative*>(panel), &renderer.value), "Create renderer");
            Require(panel->attaches == 1 && panel->chain, "Composition swap chain was not attached");
            Hr(nb_resize(renderer.value, 960, 720, 1.5f, 1.5f), "Resize");
            NbItem item{17, 0, 0, 0, 2, 0.7f, 0.3f, 0.2f, path.c_str()};
            Hr(nb_set_scene(renderer.value, &item, 1), "Set one photograph");
            path.clear(); // The DLL must already own the UTF-16 path.
            auto single = Wait(renderer.value, [](const NbStats& s) { return s.items == 1 && s.resident == 1 && !s.pending; }, "Decode one photograph");
            Require(single.failed == 0, "Generated photograph failed");
            int picked = -1;
            Hr(nb_pick(renderer.value, 480, 360, &picked), "Pick center");
            Require(picked == 17, "CPU projection does not match camera center");
            DXGI_MATRIX_3X2_F transform{};
            Hr(panel->chain->GetMatrixTransform(&transform), "Read DPI transform");
            Require(std::abs(transform._11 - 1 / 1.5f) < 0.0001f &&
                    std::abs(transform._22 - 1 / 1.5f) < 0.0001f, "Incorrect pixel/DIP transform");
            Idle(renderer.value);
            IdleFpsRegression(renderer.value);
            RefinementTests(renderer.value);

            NbStats badStats{};
            Require(FAILED(nb_get_stats(renderer.value, &badStats)), "Reject wrong stats size");
            Require(FAILED(nb_set_positions(renderer.value, nullptr, 1)), "Reject null positions");
            Require(FAILED(nb_set_filter(renderer.value, nullptr, 1)), "Reject null filter");
            Require(FAILED(nb_select(renderer.value, 999)), "Reject unknown selection ID");
            Require(FAILED(nb_resize(renderer.value, -1, 720, 1, 1)), "Reject negative dimensions");
            Require(FAILED(nb_zoom(renderer.value, NAN)), "Reject NaN camera input");

            unsigned char hidden = 0;
            uint64_t before = Stats(renderer.value).frames;
            Hr(nb_set_filter(renderer.value, &hidden, 1), "Hide photograph");
            Wait(renderer.value, [before](const NbStats& s) { return s.frames > before; }, "Apply filter");
            Hr(nb_pick(renderer.value, 480, 360, &picked), "Pick hidden photograph");
            Require(picked == -1, "Hidden photograph remained pickable");
            unsigned char visible = 1;
            Hr(nb_set_filter(renderer.value, &visible, 1), "Show photograph");
            Hr(nb_select(renderer.value, 17), "Select photograph");
            Hr(nb_set_theme(renderer.value, 1), "Light theme");
            float position[]{3.125f, 1.09375f, -0.3125f};
            before = Stats(renderer.value).frames;
            Hr(nb_set_positions(renderer.value, position, 1), "Animate layout");
            std::this_thread::sleep_for(650ms);
            Require(Stats(renderer.value).frames > before + 5, "Layout tween did not animate");
            before = Stats(renderer.value).frames;
            Hr(nb_fit(renderer.value), "Fit changed layout");
            Wait(renderer.value, [before](const NbStats& s) { return s.frames > before; }, "Apply fit");
            Idle(renderer.value);
            Hr(nb_pick(renderer.value, 480, 360, &picked), "Pick fitted photograph");
            Require(picked == 17, "Fit did not target layout");
            Hr(nb_orbit(renderer.value, 14, -8), "Orbit DIPs");
            Hr(nb_pan(renderer.value, 9, -4), "Pan DIPs");
            Hr(nb_zoom(renderer.value, 120), "Wheel zoom");
            Idle(renderer.value);

            std::wstring missing = (std::filesystem::path(argv[1]) / L"absent-nebula-smoke.jpg").wstring();
            NbItem broken[]{ {7, -0.625f, 0, 0, 1, 1, 0.1f, 0.1f, missing.c_str()},
                             {8, 0.625f, 0, 0, 1, 0.1f, 1, 0.1f, missing.c_str()} };
            wchar_t previousError[2048]{};
            Hr(nb_get_error(renderer.value, previousError, ARRAYSIZE(previousError)), "Capture prior renderer error");
            Hr(nb_set_scene(renderer.value, broken, 2), "Set missing photographs");
            Wait(renderer.value, [](const NbStats& s) { return s.items == 2 && s.failed == 2 && !s.pending; }, "Count WIC failures");
            wchar_t error[2048]{};
            Hr(nb_get_error(renderer.value, error, ARRAYSIZE(error)), "Get operational error");
            Require(wcscmp(error, previousError) == 0, "Per-photo decode failure changed renderer GetError");
            int fallbackHits = 0;
            for (const auto& missingPhoto : broken) {
                auto projected = Project(View(renderer.value), {missingPhoto.x, missingPhoto.y, missingPhoto.z});
                Hr(nb_pick(renderer.value, projected[0], projected[1], &picked), "Pick fallback density point");
                if (picked >= 0) ++fallbackHits;
            }
            Require(fallbackHits > 0, "Missing images did not retain visible fallback tiles");
            Idle(renderer.value);

            path = thumbnail.wstring();
            std::vector<NbItem> cloud;
            cloud.reserve(5000);
            for (uint32_t i = 0; i < 5000; ++i) {
                float a = i * 2.39996323f;
                float z = 1 - 2 * (i + 0.5f) / 5000;
                float radial = std::sqrt(1 - z * z) * (100 + (i % 17) * 4);
                cloud.push_back({i, std::cos(a) * radial / 32, std::sin(a) * radial / 32, z * 165 / 32, 2,
                                 0.7f, 0.4f, 0.2f, path.c_str()});
            }
            MemoryBudgetProbe budget(panel->chain.Get());
            Hr(nb_set_scene(renderer.value, cloud.data(), static_cast<int>(cloud.size())), "Set 5000 photographs");
            NbGlassRect streamingGlass[]{ {28, 28, 230, 490, 18, 0.55f}, {300, 28, 580, 70, 18, 0.55f} };
            Hr(nb_set_glass(renderer.value, streamingGlass, 2), "Warm 5000-photo stream under actual backdrop glass");
            auto saturated = Wait(renderer.value, [&](const NbStats& s) {
                budget.Sample();
                auto lod = Lod(renderer.value);
                return s.items == 5000 && lod.mosaicReady == 5000 && !s.pending;
            }, "Load every scene mosaic while bounding streamed image pools");
            Require(saturated.failed == 0, "Valid 5000-photo scene has decode failures");
            Idle(renderer.value);
            IdleFpsRegression(renderer.value);
            budget.Sample();
            Hr(nb_orbit(renderer.value, 240, 40), "Orbit saturated pool");
            std::this_thread::sleep_for(100ms);
            for (int i = 0; i < 12; ++i) {
                Hr(nb_set_scene(renderer.value, cloud.data(), static_cast<int>(cloud.size())), "Replace in-flight generation");
                std::this_thread::sleep_for(4ms);
                budget.Sample();
            }
            item.id = 42;
            item.imagePath = path.c_str();
            Hr(nb_set_scene(renderer.value, &item, 1), "Replace with final generation");
            auto final = Wait(renderer.value, [](const NbStats& s) { return s.items == 1 && s.resident == 1 && !s.pending; }, "Drain cancelled generations");
            budget.Report();
            Require(final.failed == 0, "Stale generation contaminated final scene");
            Hr(nb_set_glass(renderer.value, nullptr, 0), "Release streaming-test glass");
            Hr(nb_pick(renderer.value, 480, 360, &picked), "Pick replacement generation");
            Require(picked == 42, "Stale ID survived scene replacement");
            Hr(nb_resize(renderer.value, 0, 0, 1.5f, 1.5f), "Suspend zero-size surface");
            Idle(renderer.value);
            before = Stats(renderer.value).frames;
            Hr(nb_resize(renderer.value, 960, 720, 1.5f, 1.5f), "Resume surface");
            Wait(renderer.value, [before](const NbStats& s) { return s.frames > before; }, "Resume presents");
            {
                ComPtr<TestPanel> searchPanel;
                searchPanel.Attach(new TestPanel());
                Handle searchRenderer;
                Hr(nb_create(static_cast<ISwapChainPanelNative*>(searchPanel.Get()), &searchRenderer.value), "Create isolated search camera");
                Hr(nb_resize(searchRenderer.value, 960, 720, 1.5f, 1.5f), "Resize search fixture");
                SearchTests(searchRenderer.value, path.c_str());
                HighlightTests(searchRenderer.value, path.c_str());
            }
            Hr(nb_set_scene(renderer.value, nullptr, 0), "Clear scene");
            Wait(renderer.value, [](const NbStats& s) { return !s.items && !s.resident && !s.pending; }, "Clear all residency");
            std::cout << "PASS hardware D3D11: 5000 resident mosaics, bounded BC7 mip pools, 2 WIC failures, generation cancellation,\n"
                      << "     projection/picking, filter/selection, 500ms tween, orbit/pan/zoom/fit, DPI transform,\n"
                      << "     glass rectangles/count/clipping/bypass, hover fade/lift, focus/back, reduced motion, five pixel modes,\n"
                      << "     zero-size suspend/resume, event-driven idle, ABI NbItem=" << sizeof(NbItem)
                      << " NbStats=" << sizeof(NbStats) << " NbGlassRect=" << sizeof(NbGlassRect)
                      << " NbGlassPanel=" << sizeof(NbGlassPanel)
                      << " NbResult=" << sizeof(NbResult) << '\n';
        }
        Require(panel->detaches == 1 && !panel->chain, "Destroy did not detach composition swap chain");
        report_tests::Cache(std::filesystem::path(argv[1]));
        OriginalTests(thumbnail.c_str(), std::filesystem::path(argv[1]));
        PhotoFailureTests(thumbnail.c_str(), std::filesystem::path(argv[1]));
        NearFadeTests(thumbnail.c_str());
        Stress100K(thumbnail.c_str(), std::filesystem::path(argv[1]));
        // The readback fixture owns an extra 100K GPU scene; measure the renderer
        // budget before that separate device can populate driver allocation caches.
        lod_tests::MotionReadback(std::filesystem::path(argv[1]));
        if (withLibrary) {
            DecodeAudit(argv[3], argv[1]);
            RealLibraryTests(argv[3]);
        }
        std::filesystem::remove(thumbnail);
        CoUninitialize();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        if (!thumbnail.empty()) { std::error_code ignored; std::filesystem::remove(thumbnail, ignored); }
        CoUninitialize();
        return 1;
    }
}
