#define NEBULA_RENDER_EXPORTS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "NebulaRender.h"
#include "Shaders.h"
#include "MosaicAverages.h"
#include "RenderLimits.h"
#include "GlassMaterial.h"
#include "Highlight.h"
#include "BoundedQueue.h"
#include "PickGrid.h"
#include "OriginalLoader.h"
#include "DepthEffects.h"
#include "MipCache.h"
#include "ThumbnailDecoder.h"
#include "LodPolicy.h"
#include "RenderRecords.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <microsoft.ui.xaml.media.dxinterop.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <vector>
#include <bit>
#include "ShaderBytecode.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using Clock = std::chrono::steady_clock;

namespace {
using namespace nebula_limits;
constexpr float Fov = 48.0f * XM_PI / 180.0f;
constexpr float TileHalfSize = 0.5f;
constexpr float NearPlane = 0.001f;
constexpr float FarPlane = 10000000.0f;

struct HrFailure {
    HRESULT hr;
    const wchar_t* operation;
};

void Check(HRESULT hr, const wchar_t* operation) {
    if (FAILED(hr)) throw HrFailure{hr, operation};
}

bool Finite(float x) { return std::isfinite(x); }
float Srgb(float x) { return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f); }
float Smooth(float value) { value = std::clamp(value, 0.0f, 1.0f); return value * value * (3 - 2 * value); }
float SmoothRange(float low, float high, float value) { return Smooth((value - low) / (high - low)); }
bool Approach(float& value, float target, float step) {
    float difference = target - value;
    if (std::abs(difference) <= step) { value = target; return false; }
    value += std::copysign(step, difference);
    return true;
}

struct ColdState {
    XMFLOAT3 from{}, target{}, base{};
    float alphaFrom = 1, alphaTarget = 1, scaleFrom = 1, scaleTarget = 1, overlayFrom = 0, overlayTarget = 0;
    std::array<int, 4> imageSlots{-1,-1,-1,-1};
    HRESULT originalError = S_OK;
};
static_assert(sizeof(ColdState) == 80);

struct alignas(16) Item {
    uint32_t id = 0;
    XMFLOAT3 position{};
    float aspect = 1;
    uint32_t packedColor = 0xff000000;
    ColdState* cold = nullptr;
    int quality = -1;
    float hover = 0;
    float highlight = 0, highlightTarget = 0;
    float selection = 0, alpha = 1;
    float scale = 1;
    float overlay = 0;
    float visibility = 1, contrast = 1;
    int tier = 0;
    uint32_t tierSince = UINT32_MAX - 99;
    float attention = 1, edge = 3, viewX = 0, viewY = 0, viewZ = 0, inverseZ = 0;
    int repA = 0, repB = 0;
    float representationMix = 1;
    float mosaicMix = 0, thumbnailMix = 0;
    bool mosaicReady : 1 = false, mosaicComplete : 1 = false, artReady : 1 = false, cacheWarningLogged : 1 = false;
    bool matched : 1 = false, onScreen : 1 = false;
    bool failed : 1 = false, visible : 1 = true, hasOriginal : 1 = false;
    uint8_t failedTiers = 0, imageLoading = 0;
};
static_assert(sizeof(Item) <= 128);

struct SceneSnapshot {
    std::vector<Item> items;
    std::shared_ptr<const std::vector<std::wstring>> paths;
    std::shared_ptr<const std::vector<std::wstring>> originals;
    std::shared_ptr<const std::vector<uint64_t>> versions;
    std::shared_ptr<std::vector<ColdState>> cold;
};

struct Size {
    int width = 1, height = 1;
    float scaleX = 1, scaleY = 1;
};

struct Commands {
    std::optional<SceneSnapshot> scene;
    std::optional<std::vector<XMFLOAT3>> positions;
    std::optional<std::vector<unsigned char>> filter;
    std::optional<Size> size;
    std::optional<int> selection, theme;
    std::optional<int> hover, focus, reducedMotion, pixels;
    std::optional<std::pair<int, int>> depthOptions;
    std::optional<std::vector<NbGlassPanel>> glass;
    std::optional<std::vector<NbResult>> results;
    std::optional<std::vector<std::pair<uint32_t, float>>> highlight;
    std::optional<std::vector<NbLine>> lines;
    std::optional<std::vector<uint32_t>> focusSet;
    float orbitX = 0, orbitY = 0, panX = 0, panY = 0, zoom = 0;
    bool fit = false, back = false, clearResults = false, dirty = false;
};

struct Request {
    uint64_t generation = 0;
    size_t index = 0, count = 1;
    int tier = 1;
    std::shared_ptr<const std::vector<std::wstring>> paths;
    std::shared_ptr<const std::vector<uint64_t>> versions;
    std::shared_ptr<nebula_cache::Store> cache;
};

struct MosaicRecord {
    size_t index = 0;
    HRESULT hr = S_OK;
    HRESULT cacheError = S_OK;
    const wchar_t* operation = L"Decode mosaic";
    float aspect = 1;
    std::array<unsigned char, MosaicBytesPerSlice> pixels{};
};

struct Decoded {
    uint64_t generation = 0;
    size_t index = 0;
    HRESULT hr = S_OK;
    HRESULT cacheError = S_OK;
    const wchar_t* operation = L"Decode photograph";
    float aspect = 1;
    XMFLOAT4 uv{};
    std::vector<unsigned char> pixels;
    std::vector<unsigned char> mosaicPixels;
    int tier = 1;
    std::vector<MosaicRecord> mosaics;
    std::array<uint32_t, 11> mipOffsets{};
    uint32_t mipLevels = 0;
};

struct Slot {
    int owner = -1;
    uint64_t lastUse = 0;
    XMFLOAT4 uv{};
    uint32_t version = 0;
    Clock::time_point loadedAt{};
};

struct ImagePool {
    struct Candidate {
        int index;
        uint64_t lastUse;
        bool orphan;
        static bool Later(const Candidate& a, const Candidate& b) {
            if (a.orphan != b.orphan) return !a.orphan;
            return a.lastUse != b.lastUse ? a.lastUse > b.lastUse : a.index > b.index;
        }
    };
    uint32_t edge = 0, mipLevels = 0;
    size_t unusedSlot = 0;
    uint64_t reuseFrame = UINT64_MAX;
    std::vector<Candidate> reusable;
    std::vector<Slot> slots;
    std::vector<ComPtr<ID3D11Texture2D>> textures;
    std::vector<ComPtr<ID3D11ShaderResourceView>> views;
};

using Instance = nebula_records::Tile;

struct LineVertex {
    XMFLOAT3 position;
    float alpha;
};
static_assert(sizeof(LineVertex) == 16);

struct Constants {
    XMFLOAT4X4 view, projection;
    XMFLOAT4 viewport;
};
static_assert(sizeof(Constants) % 16 == 0);

struct CameraState {
    float yaw = 0.35f, pitch = 0.18f, distance = 100;
    XMFLOAT3 target{};
};

struct GlassConstants {
    XMFLOAT4 outputInfo{}, blurInfo{};
    std::array<XMFLOAT4, 8> bounds{}, material{};
    XMFLOAT4 displayScale{};
};
static_assert(sizeof(GlassConstants) == 304);

struct ColorTarget {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11ShaderResourceView> view;
};

class Renderer {
public:
    ComPtr<ISwapChainPanelNative> panel;

    ~Renderer() {
        {
            std::lock_guard lock(commandMutex_);
            stop_.store(true);
            wake_.notify_all();
        }
        {
            std::lock_guard lock(decodeMutex_);
            decodeWake_.notify_all();
            resultSpace_.notify_all();
        }
        if (renderThread_.joinable()) renderThread_.join();
        for (auto& worker : workers_) if (worker.joinable()) worker.join();
        if (frameReady_) CloseHandle(frameReady_);
    }

    HRESULT Start() {
        renderThread_ = std::thread([this] { Run(); });
        std::unique_lock lock(commandMutex_);
        wake_.wait(lock, [this] { return initialized_; });
        return fatal_.load();
    }

    HRESULT Attach() {
        HRESULT hr = panel->SetSwapChain(swapChain_.Get());
        if (FAILED(hr)) { Error(hr, L"WinUI 3 ISwapChainPanelNative::SetSwapChain", true); return hr; }
        std::lock_guard lock(commandMutex_);
        attached_ = true;
        commands_.dirty = true;
        wake_.notify_one();
        return S_OK;
    }

    HRESULT Scene(const NbItem* input, int count, const NbItemV2* extended = nullptr) {
        if (count < 0 || static_cast<size_t>(count) > MaxItems || (count && !input && !extended))
            return E_INVALIDARG;
        std::vector<Item> copy;
        copy.reserve(count);
        auto paths = std::make_shared<std::vector<std::wstring>>();
        paths->reserve(count);
        auto originals = std::make_shared<std::vector<std::wstring>>();
        auto versions = std::make_shared<std::vector<uint64_t>>();
        auto cold = std::make_shared<std::vector<ColdState>>(count);
        originals->reserve(count); versions->reserve(count);
        std::unordered_set<uint32_t> ids;
        size_t pathBytes = 0, originalBytes = 0;
        for (int i = 0; i < count; ++i) {
            const auto& src = extended ? extended[i].item : input[i];
            if (src.id > INT_MAX || !ids.insert(src.id).second ||
                !Finite(src.x) || !Finite(src.y) || !Finite(src.z) ||
                std::abs(src.x) > 1000000 || std::abs(src.y) > 1000000 || std::abs(src.z) > 1000000 ||
                !Finite(src.aspect) || src.aspect <= 0 ||
                !Finite(src.r) || !Finite(src.g) || !Finite(src.b)) return E_INVALIDARG;
            Item item;
            item.cold = &(*cold)[i];
            item.id = src.id;
            item.position = item.cold->from = item.cold->target = item.cold->base = {src.x, src.y, src.z};
            item.aspect = std::clamp(src.aspect, 0.01f, 100.0f);
            item.packedColor = 0xff000000u | static_cast<uint32_t>(std::lround(std::clamp(src.r, 0.0f, 1.0f) * 255)) |
                (static_cast<uint32_t>(std::lround(std::clamp(src.g, 0.0f, 1.0f) * 255)) << 8) |
                (static_cast<uint32_t>(std::lround(std::clamp(src.b, 0.0f, 1.0f) * 255)) << 16);
            if (src.imagePath) {
                size_t length = wcsnlen_s(src.imagePath, 32768);
                if (length >= 32768) return E_INVALIDARG;
                pathBytes += (length + 1) * sizeof(wchar_t);
                if (pathBytes > PathBudget) return E_OUTOFMEMORY;
                paths->emplace_back(src.imagePath, length);
            } else paths->emplace_back();
            const wchar_t* original = extended ? extended[i].originalPath : nullptr;
            size_t originalLength = original ? wcsnlen_s(original, 32768) : 0;
            if (originalLength >= 32768) return E_INVALIDARG;
            originalBytes += (originalLength + 1) * sizeof(wchar_t);
            if (originalBytes > PathBudget) return E_OUTOFMEMORY;
            originals->emplace_back(original ? original : L"", originalLength);
            item.hasOriginal = originalLength != 0;
            versions->push_back(extended ? extended[i].contentVersion : 0);
            copy.push_back(std::move(item));
        }
        return Command([&](Commands& c) {
            acceptedCount_ = count;
            acceptedHover_ = -1;
            acceptedIds_ = std::move(ids);
            c.scene = SceneSnapshot{std::move(copy), std::move(paths), std::move(originals), std::move(versions), std::move(cold)};
            c.positions.reset();
            c.filter.reset();
            c.selection = -1;
            c.hover = -1;
            c.focus.reset();
            c.focusSet.reset();
            c.highlight.reset();
            c.lines.reset();
            c.results.reset();
            c.clearResults = false;
            c.back = false;
            c.fit = true;
            c.orbitX = c.orbitY = c.panX = c.panY = c.zoom = 0;
        });
    }

    HRESULT Positions(const float* xyz, int count) {
        if (count < 0 || static_cast<size_t>(count) > MaxItems || (count && !xyz)) return E_INVALIDARG;
        std::vector<XMFLOAT3> copy;
        copy.reserve(count);
        for (int i = 0; i < count; ++i) {
            XMFLOAT3 p{xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]};
            if (!Finite(p.x) || !Finite(p.y) || !Finite(p.z) ||
                std::abs(p.x) > 1000000 || std::abs(p.y) > 1000000 || std::abs(p.z) > 1000000) return E_INVALIDARG;
            copy.push_back(p);
        }
        std::lock_guard lock(commandMutex_);
        if (count != acceptedCount_) return E_INVALIDARG;
        if (FAILED(fatal_)) return fatal_.load();
        commands_.positions = std::move(copy);
        commands_.dirty = true;
        wake_.notify_one();
        return S_OK;
    }

    HRESULT Filter(const unsigned char* mask, int count) {
        if (count < 0 || static_cast<size_t>(count) > MaxItems || (count && !mask)) return E_INVALIDARG;
        std::vector<unsigned char> copy;
        if (count) copy.assign(mask, mask + count);
        std::lock_guard lock(commandMutex_);
        if (count != acceptedCount_) return E_INVALIDARG;
        if (FAILED(fatal_)) return fatal_.load();
        commands_.filter = std::move(copy);
        commands_.dirty = true;
        wake_.notify_one();
        return S_OK;
    }

    HRESULT Select(int id) {
        std::lock_guard lock(commandMutex_);
        if (id < -1 || (id >= 0 && !acceptedIds_.contains(static_cast<uint32_t>(id)))) return E_INVALIDARG;
        if (FAILED(fatal_)) return fatal_.load();
        commands_.selection = id;
        commands_.dirty = true;
        wake_.notify_one();
        return S_OK;
    }

    HRESULT Hover(int id) {
        std::lock_guard lock(commandMutex_);
        if (id < -1 || (id >= 0 && !acceptedIds_.contains(static_cast<uint32_t>(id)))) return E_INVALIDARG;
        if (FAILED(fatal_)) return fatal_.load();
        if (acceptedHover_ == id) return S_OK;
        acceptedHover_ = id;
        commands_.hover = id;
        commands_.dirty = true;
        wake_.notify_one();
        return S_OK;
    }

    HRESULT Focus(int id) {
        std::lock_guard lock(commandMutex_);
        if (id < 0 || !acceptedIds_.contains(static_cast<uint32_t>(id))) return E_INVALIDARG;
        if (FAILED(fatal_)) return fatal_.load();
        commands_.focus = id;
        commands_.focusSet.reset();
        commands_.fit = commands_.back = false;
        commands_.dirty = true;
        wake_.notify_one();
        return S_OK;
    }

    HRESULT Highlight(const uint32_t* ids, const float* weights, int count) {
        if (count < 0 || static_cast<size_t>(count) > MaxItems || (count && (!ids || !weights))) return E_INVALIDARG;
        std::vector<std::pair<uint32_t, float>> copy;
        std::unordered_set<uint32_t> unique;
        copy.reserve(count);
        for (int i = 0; i < count; ++i) {
            if (!Finite(weights[i]) || weights[i] < 0 || weights[i] > 1 || !unique.insert(ids[i]).second)
                return E_INVALIDARG;
            copy.emplace_back(ids[i], weights[i]);
        }
        std::lock_guard lock(commandMutex_);
        if (FAILED(fatal_)) return fatal_.load();
        for (const auto& [id, weight] : copy) {
            (void)weight;
            if (!acceptedIds_.contains(id)) return E_INVALIDARG;
        }
        commands_.highlight = std::move(copy);
        if (count == 0) commands_.lines = std::vector<NbLine>{};
        commands_.dirty = true;
        wake_.notify_one();
        return S_OK;
    }

    HRESULT Lines(const NbLine* lines, int count) {
        if (count < 0 || static_cast<size_t>(count) > nebula_highlight::MaxLines || (count && !lines)) return E_INVALIDARG;
        std::vector<NbLine> copy;
        if (count) copy.assign(lines, lines + count);
        for (const auto& line : copy)
            if (!Finite(line.weight) || line.weight < 0 || line.weight > 1) return E_INVALIDARG;
        std::lock_guard lock(commandMutex_);
        if (FAILED(fatal_)) return fatal_.load();
        for (const auto& line : copy)
            if (!acceptedIds_.contains(line.a) || !acceptedIds_.contains(line.b)) return E_INVALIDARG;
        commands_.lines = std::move(copy);
        commands_.dirty = true;
        wake_.notify_one();
        return S_OK;
    }

    HRESULT FocusSet(const uint32_t* ids, int count) {
        if (count < 0 || static_cast<size_t>(count) > MaxItems || (count && !ids)) return E_INVALIDARG;
        std::vector<uint32_t> copy;
        std::unordered_set<uint32_t> unique;
        copy.reserve(count);
        for (int i = 0; i < count; ++i) if (unique.insert(ids[i]).second) copy.push_back(ids[i]);
        std::lock_guard lock(commandMutex_);
        if (FAILED(fatal_)) return fatal_.load();
        for (uint32_t id : copy) if (!acceptedIds_.contains(id)) return E_INVALIDARG;
        if (copy.empty()) return S_OK;
        commands_.focusSet = std::move(copy);
        commands_.focus.reset();
        commands_.back = commands_.fit = false;
        commands_.dirty = true;
        wake_.notify_one();
        return S_OK;
    }

    HRESULT Results(const NbResult* input, int count) {
        if (count < 0 || static_cast<size_t>(count) > MaxItems || (count && !input)) return E_INVALIDARG;
        std::vector<NbResult> copy;
        if (count) copy.assign(input, input + count);
        std::unordered_set<uint32_t> ids;
        for (const auto& result : copy) {
            if (!ids.insert(result.id).second || !Finite(result.x) || !Finite(result.y) ||
                !Finite(result.z) || !Finite(result.weight) || std::abs(result.x) > 1000000 ||
                std::abs(result.y) > 1000000 || std::abs(result.z) > 1000000) return E_INVALIDARG;
        }
        std::lock_guard lock(commandMutex_);
        if (FAILED(fatal_)) return fatal_.load();
        for (const auto& result : copy) if (!acceptedIds_.contains(result.id)) return E_INVALIDARG;
        commands_.results = std::move(copy);
        commands_.clearResults = false;
        acceptedHover_ = -1;
        commands_.hover = -1;
        commands_.dirty = true;
        wake_.notify_one();
        return S_OK;
    }

    HRESULT ClearResults() {
        return Command([this](Commands& c) {
            c.clearResults = true; c.results.reset(); c.hover = -1; acceptedHover_ = -1;
        });
    }

    HRESULT Glass(const NbGlassRect* input, int count) {
        if (count < 0 || count > 8 || (count && !input)) return E_INVALIDARG;
        std::array<NbGlassPanel, 8> panels{};
        for (int i = 0; i < count; ++i) {
            const auto& rect = input[i];
            panels[i] = {rect.x, rect.y, rect.width, rect.height, rect.radius, rect.tint, 1};
        }
        return GlassPanels(panels.data(), count);
    }

    HRESULT GlassPanels(const NbGlassPanel* input, int count) {
        if (count < 0 || count > 8 || (count && !input)) return E_INVALIDARG;
        std::vector<NbGlassPanel> copy;
        copy.reserve(count);
        for (int i = 0; i < count; ++i) {
            auto rect = input[i];
            if (!Finite(rect.x) || !Finite(rect.y) || !Finite(rect.width) || !Finite(rect.height) ||
                !Finite(rect.radius) || !Finite(rect.tint) || !Finite(rect.opacity) ||
                std::abs(rect.x) > 32768 || std::abs(rect.y) > 32768 ||
                rect.width <= 0 || rect.height <= 0 || rect.width > 32768 || rect.height > 32768 ||
                rect.radius < 0 || rect.radius > 16384 || rect.tint < 0 || rect.tint > 1 ||
                rect.opacity < 0 || rect.opacity > 1)
                return E_INVALIDARG;
            rect.radius = std::min(rect.radius, std::min(rect.width, rect.height) * 0.5f);
            copy.push_back(rect);
        }
        std::lock_guard lock(commandMutex_);
        if (FAILED(fatal_)) return fatal_.load();
        if (copy.size() == acceptedGlass_.size() &&
            (copy.empty() || memcmp(copy.data(), acceptedGlass_.data(), copy.size() * sizeof(NbGlassPanel)) == 0))
            return S_OK;
        acceptedGlass_ = copy;
        commands_.glass = std::move(copy);
        commands_.dirty = true;
        wake_.notify_one();
        return S_OK;
    }

    template<class F> HRESULT Command(F&& f) {
        std::lock_guard lock(commandMutex_);
        if (FAILED(fatal_)) return fatal_.load();
        f(commands_);
        commands_.dirty = true;
        wake_.notify_one();
        return S_OK;
    }

    HRESULT Pick(float x, float y, int* id, int* tier = nullptr) {
        if (!id || !Finite(x) || !Finite(y)) return E_INVALIDARG;
        *id = -1;
        if (tier) *tier = -1;
        auto started = Clock::now();
        std::lock_guard lock(statusMutex_);
        float best = std::numeric_limits<float>::max();
        uint32_t bestIndex = 0;
        for (const auto& chunk : publishedPicks_) chunk.grid.Visit(x, y, [&](uint32_t index) {
            const auto& hit = chunk.hits[index];
            float dx = (x - hit.x) / hit.halfX, dy = (y - hit.y) / hit.halfY;
            if (std::abs(dx) > 1 || std::abs(dy) > 1) return;
            if (!Contains(hit, x, y)) return;
            float depth = hit.depth;
            if (depth < best || (depth == best && hit.order >= bestIndex)) {
                best = depth; *id = hit.id;
                bestIndex = hit.order;
                if (tier) *tier = hit.tier;
            }
        });
        lodSnapshot_.pickMs = std::chrono::duration<float, std::milli>(Clock::now() - started).count();
        return fatal_.load();
    }

    HRESULT View(NbView* out) {
        if (!out || out->size != sizeof(NbView)) return E_INVALIDARG;
        std::lock_guard lock(statusMutex_);
        *out = viewSnapshot_;
        if (FAILED(fatal_)) return fatal_.load();
        return viewReady_ ? S_OK : S_FALSE;
    }

    HRESULT Stats(NbStats* out) {
        if (!out || out->size != sizeof(NbStats)) return E_INVALIDARG;
        std::lock_guard lock(statusMutex_);
        *out = stats_;
        if (Clock::now() - lastPresent_ > std::chrono::milliseconds(500)) out->fps = 0;
        return fatal_.load();
    }

    HRESULT LodStats(NbLodStats* out) {
        if (!out || out->size != sizeof(NbLodStats)) return E_INVALIDARG;
        std::lock_guard lock(statusMutex_);
        *out = lodSnapshot_;
        return fatal_.load();
    }

    HRESULT FocusStats(NbFocusStats* out) {
        if (!out || out->size != sizeof(NbFocusStats)) return E_INVALIDARG;
        std::lock_guard lock(statusMutex_);
        *out = focusSnapshot_;
        return fatal_.load();
    }
    HRESULT Diagnostics(NbFrameDiagnostics* out) {
        if (!out || out->size != sizeof(NbFrameDiagnostics)) return E_INVALIDARG;
        std::lock_guard lock(statusMutex_);
        *out = diagnosticsSnapshot_;
        return fatal_.load();
    }

    HRESULT GetError(wchar_t* buffer, int capacity) {
        if (!buffer || capacity <= 0) return E_INVALIDARG;
        std::lock_guard lock(statusMutex_);
        size_t length = std::min(error_.size(), static_cast<size_t>(capacity - 1));
        std::copy_n(error_.data(), length, buffer);
        buffer[length] = L'\0';
        return length == error_.size() ? S_OK : HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }

    void Error(HRESULT hr, const wchar_t* operation, bool fatal = false) noexcept {
        if (!fatal && FAILED(fatal_.load())) return;
        if (fatal) fatal_.store(hr);
        try {
            wchar_t code[32]{};
            swprintf_s(code, L" (HRESULT 0x%08X)", static_cast<unsigned>(hr));
            std::lock_guard lock(statusMutex_);
            error_ = std::wstring(operation) + code;
        } catch (...) {}
    }

private:
    std::mutex commandMutex_, statusMutex_, decodeMutex_;
    std::condition_variable wake_, decodeWake_, resultSpace_;
    std::atomic<bool> stop_{false};
    std::atomic<HRESULT> fatal_{S_OK};
    std::atomic<uint64_t> generation_{0};
    std::atomic<size_t> readyResults_{0};
    bool initialized_ = false, attached_ = false;
    int acceptedCount_ = 0;
    int acceptedHover_ = -1;
    std::vector<NbGlassPanel> acceptedGlass_;
    std::unordered_set<uint32_t> acceptedIds_;
    Commands commands_;
    std::thread renderThread_;
    HANDLE frameReady_ = nullptr;
    bool resumedFromIdle_ = true;
    bool sceneResourcesDirty_ = false;
    std::array<std::thread, 2> workers_;
    BoundedQueue<Request, RequestLimit> requests_;
    BoundedQueue<Request, MosaicJobs> mosaicRequests_;
    BoundedQueue<Decoded, ResultLimit> results_;
    NbStats stats_{sizeof(NbStats)};
    NbView viewSnapshot_{sizeof(NbView)};
    bool viewReady_ = false;
    std::wstring error_;
    NbLodStats lodSnapshot_{sizeof(NbLodStats)}, frameLod_{sizeof(NbLodStats)};
    NbFocusStats focusSnapshot_{sizeof(NbFocusStats), -1}, focusState_{sizeof(NbFocusStats), -1};
    NbFrameDiagnostics diagnostics_{sizeof(NbFrameDiagnostics)}, diagnosticsSnapshot_{sizeof(NbFrameDiagnostics)};
    std::unique_ptr<nebula_original::Loader> originalLoader_;
    std::optional<nebula_original::Image> pendingOriginal_;
    UINT originalUploadRow_ = 0;
    ComPtr<ID3D11Texture2D> originalUploadTexture_;
    std::shared_ptr<const std::vector<std::wstring>> originals_;
    std::shared_ptr<const std::vector<uint64_t>> contentVersions_;
    uint64_t originalSerial_ = 0;
    int requestedOriginal_ = -1;
    std::array<float, 4> requestedUv_{0,0,1,1};
    XMFLOAT4 originalUv_{0,0,1,1};
    float lensDistance_ = 100;
    bool atmosphere_ = false, focusDof_ = true;
    float originalMix_ = 0;
    bool originalUsedThisFrame_ = false;
    Clock::time_point lastPresent_{};

    std::vector<Item> items_;
    std::shared_ptr<std::vector<ColdState>> cold_;
    std::unordered_map<uint32_t, size_t> itemIndex_;
    std::array<ImagePool, 4> imagePools_;
    std::shared_ptr<nebula_cache::Store> cache_;
    std::shared_ptr<const std::vector<std::wstring>> paths_;
    size_t nextMosaic_ = 0;
    size_t nextArt_ = 0, artCapacity_ = 0;
    bool artRequested_ = false;
    uint32_t pendingJobs_ = 0, mosaicJobs_ = 0, mosaicReady_ = 0, mosaicDone_ = 0, failed_ = 0;
    uint32_t frameResident_ = 0;
    uint64_t flips_ = 0, uploads_ = 0, evictions_ = 0;
    nebula_lod::Caps caps_ = nebula_lod::Floors;
    float sizeCap0_ = nebula_lod::Floors[0];
    std::vector<float> sizeEdges_;
    std::vector<Instance> instanceShadow_;
    std::vector<uint32_t> instanceOrder_, orderShadow_;
    std::vector<uint64_t> depthKeys_, orderScratch_;
    size_t instanceDirtyFirst_ = SIZE_MAX, instanceDirtyLast_ = 0;
    std::vector<NbLine> lines_;
    std::vector<LineVertex> lineVertices_;
    struct Want { float priority; size_t index; int tier; int category = 0; };
    struct FrameChunk {
        size_t begin = 0, end = 0;
        std::vector<uint32_t> tiles;
        std::vector<Hit> hits;
        PickGrid grid;
        std::array<uint32_t, 64> histogram{};
        std::array<Want, RequestsPerFrame> wants{};
        size_t wantCount = 0;
        NbLodStats lod{sizeof(NbLodStats)};
        bool moving = false, originalUsed = false;
        size_t instanceFirst = SIZE_MAX, instanceLast = 0;
        size_t submittedDots = 0;
        std::exception_ptr failure;
    };
    struct PickChunk { std::vector<Hit> hits; PickGrid grid; };
    size_t framePartitions_ = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u);
    std::vector<ULONG> frameCpuSets_;
    std::vector<FrameChunk> chunks_ = std::vector<FrameChunk>(framePartitions_);
    std::vector<PickChunk> publishedPicks_ = std::vector<PickChunk>(framePartitions_);
    std::vector<std::thread> frameWorkers_ = std::vector<std::thread>(framePartitions_ - 1);
    std::mutex frameMutex_;
    std::condition_variable frameWake_, frameDone_;
    uint64_t frameEpoch_ = 0;
    bool emitPass_ = false;
    unsigned workersPending_ = 0;
    bool stopFrameWorkers_ = false;
    Clock::time_point prepareTime_{};
    std::array<bool, 4> canAdmitFrame_{};
    size_t tileCount_ = 0, submittedDots_ = 0;
    std::array<Want, RequestsPerFrame> wants_{};
    size_t wantCount_ = 0;
    Size size_;
    bool light_ = false, animating_ = false, cameraAnimating_ = false, effectsAnimating_ = false;
    bool reducedMotion_ = false;
    bool resultsActive_ = false;
    std::optional<CameraState> searchCamera_;
    float selectionMix_ = 0, morphDuration_ = 0.5f;
    float highlightMix_ = 0;
    bool highlightActive_ = false;
    int selected_ = -1, hovered_ = -1, pixels_ = 0;
    float yaw_ = 0.35f, pitch_ = 0.18f, distance_ = 100, zoomPending_ = 0;
    XMFLOAT3 target_{};
    CameraState desired_;
    std::deque<CameraState> history_;
    std::vector<NbGlassPanel> glass_;
    Clock::time_point animationStart_{};
    Clock::time_point visualTime_{};
    Clock::time_point hoverSince_{};
    Clock::time_point frameStarted_{};
    float frameDelta_ = 1.0f / 60;
    uint64_t frames_ = 0;
    float fps_ = 0;
    XMMATRIX view_{}, projection_{};
    XMFLOAT2 projectionPixels_{}, worldPerPixel_{};

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain2> swapChain_;
    ComPtr<ID3D11RenderTargetView> targetView_;
    ComPtr<ID3D11DepthStencilView> depthView_;
    ComPtr<ID3D11Texture2D> originalTexture_;
    ComPtr<ID3D11ShaderResourceView> originalView_;
    ComPtr<ID3D11Query> originalRetire_;
    bool originalRetiring_ = false;
    std::array<ComPtr<ID3D11Buffer>, MosaicChunks> mosaicBuffers_;
    std::array<ComPtr<ID3D11ShaderResourceView>, MosaicChunks> mosaicViews_;
    ComPtr<ID3D11Buffer> artBuffer_;
    ComPtr<ID3D11ShaderResourceView> artView_;
    size_t mosaicCapacity_ = 0;
    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11PixelShader> pixelShader_;
    ComPtr<ID3D11PixelShader> opaqueDepthShader_;
    ComPtr<ID3D11VertexShader> lineVertexShader_;
    ComPtr<ID3D11PixelShader> linePixelShader_;
    ComPtr<ID3D11InputLayout> lineLayout_;
    ComPtr<ID3D11Buffer> lineBuffer_;
    ComPtr<ID3D11DepthStencilState> lineDepth_;
    ComPtr<ID3D11InputLayout> layout_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11Buffer> sceneLens_, depthLens_;
    std::array<ComPtr<ID3D11Buffer>, 1> instanceBuffers_;
    ComPtr<ID3D11ShaderResourceView> instanceView_, orderView_;
    ComPtr<ID3D11Buffer> orderBuffer_;
    size_t instanceCapacity_ = 0;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11SamplerState> pointSampler_;
    ComPtr<ID3D11RasterizerState> rasterizer_;
    ComPtr<ID3D11DepthStencilState> depthState_;
    ComPtr<ID3D11DepthStencilState> noDepth_;
    ComPtr<ID3D11BlendState> tileBlend_;
    ColorTarget linearDepth_, protectionTarget_, dofHalfA_, dofHalfB_, dofOutput_;
    ComPtr<ID3D11PixelShader> depthDown_, depthBlur_, depthResolve_;
    struct GpuTimer {
        ComPtr<ID3D11Query> disjoint, start, end;
        uint64_t frame = 0;
        bool pending = false;
    };
    std::array<GpuTimer, 4> gpuTimers_;
    GpuTimer* currentTimer_ = nullptr;
    float gpuMs_ = 0;
    uint64_t gpuFrame_ = 0;
    ComPtr<ID3D11VertexShader> fullscreenShader_;
    ComPtr<ID3D11PixelShader> downsampleShader_, blurShader_, compositeShader_;
    ComPtr<ID3D11Buffer> glassConstants_;
    ColorTarget sceneTarget_, blurA_, blurB_;
    UINT blurWidth_ = 1, blurHeight_ = 1;

    void InitializeGraphics() {
        D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0}, obtained;
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
              D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
              &device_, &obtained, &context_), L"D3D11CreateDevice (hardware; no silent WARP fallback)");
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        Check(device_.As(&dxgiDevice), L"Query IDXGIDevice");
        Check(dxgiDevice->GetAdapter(&adapter), L"Get DXGI adapter");
        Check(adapter->GetParent(IID_PPV_ARGS(&factory)), L"Get DXGI factory");
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = desc.Height = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        ComPtr<IDXGISwapChain1> chain;
        Check(factory->CreateSwapChainForComposition(device_.Get(), &desc, nullptr, &chain),
              L"CreateSwapChainForComposition");
        Check(chain.As(&swapChain_), L"Query IDXGISwapChain2");
        Check(swapChain_->SetMaximumFrameLatency(1), L"Set swap-chain frame latency");
        frameReady_ = swapChain_->GetFrameLatencyWaitableObject();
        if (!frameReady_) throw HrFailure{E_FAIL, L"Get frame-latency wait handle"};

        ComPtr<ID3DBlob> vs, ps, messages;
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
        Check(CompileShaderBytes(PhotoShader, strlen(PhotoShader), "Nebula embedded shader", nullptr, nullptr,
              "VS", "vs_5_0", flags, 0, &vs, &messages), L"Compile billboard vertex shader");
        messages.Reset();
        Check(CompileShaderBytes(PhotoShader, strlen(PhotoShader), "Nebula embedded shader", nullptr, nullptr,
              "PS", "ps_5_0", flags, 0, &ps, &messages), L"Compile photograph pixel shader");
        Check(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertexShader_), L"CreateVertexShader");
        Check(device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixelShader_), L"CreatePixelShader");
        ps.Reset(); messages.Reset();
        Check(CompileShaderBytes(PhotoShader, strlen(PhotoShader), "Opaque interior depth", nullptr, nullptr,
              "OpaqueDepthPS", "ps_5_0", flags, 0, &ps, &messages), L"Load opaque-interior depth shader");
        Check(device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &opaqueDepthShader_), L"Create opaque-interior shader");
        vs.Reset(); ps.Reset(); messages.Reset();
        Check(CompileShaderBytes(LineShader, strlen(LineShader), "Nebula constellation lines", nullptr, nullptr,
              "LineVS", "vs_5_0", flags, 0, &vs, &messages), L"Compile constellation line vertex shader");
        messages.Reset();
        Check(CompileShaderBytes(LineShader, strlen(LineShader), "Nebula constellation lines", nullptr, nullptr,
              "LinePS", "ps_5_0", flags, 0, &ps, &messages), L"Compile constellation line pixel shader");
        Check(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &lineVertexShader_), L"Create line vertex shader");
        Check(device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &linePixelShader_), L"Create line pixel shader");
        D3D11_INPUT_ELEMENT_DESC lineElements[]{
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}
        };
        Check(device_->CreateInputLayout(lineElements, ARRAYSIZE(lineElements), vs->GetBufferPointer(), vs->GetBufferSize(), &lineLayout_),
              L"Create line input layout");
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = static_cast<UINT>(nebula_highlight::MaxLines * 2 * sizeof(LineVertex));
        buffer.Usage = D3D11_USAGE_DYNAMIC;
        buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        Check(device_->CreateBuffer(&buffer, nullptr, &lineBuffer_), L"Create bounded 512-line buffer");
        buffer.ByteWidth = sizeof(Constants);
        buffer.Usage = D3D11_USAGE_DYNAMIC;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        Check(device_->CreateBuffer(&buffer, nullptr, &constants_), L"Create camera buffer");
        buffer.ByteWidth = sizeof(GlassConstants);
        Check(device_->CreateBuffer(&buffer, nullptr, &glassConstants_), L"Create glass constants");
        buffer.ByteWidth = sizeof(XMFLOAT4) * 2;
        Check(device_->CreateBuffer(&buffer, nullptr, &sceneLens_), L"Create atmospheric/focus constants");
        Check(device_->CreateBuffer(&buffer, nullptr, &depthLens_), L"Create depth-effect constants");
        EnsureInstanceBuffers(0);

        for (size_t level = 0; level < imagePools_.size(); ++level) {
            auto& pool = imagePools_[level];
            pool.edge = MipEdges[level];
            pool.mipLevels = 1;
            for (uint32_t edge = pool.edge; edge > 1; edge /= 2) ++pool.mipLevels;
            pool.slots.resize(MipSlots[level]);
            pool.reusable.reserve(MipSlots[level]);
            size_t arrays = (MipSlots[level] + 2047) / 2048;
            pool.textures.resize(arrays); pool.views.resize(arrays);
            for (size_t array = 0; array < arrays; ++array) {
                D3D11_TEXTURE2D_DESC texture{};
                texture.Width = texture.Height = pool.edge;
                texture.ArraySize = std::min(2048u, MipSlots[level] - static_cast<uint32_t>(array) * 2048);
                texture.MipLevels = pool.mipLevels;
                texture.Format = DXGI_FORMAT_BC7_UNORM_SRGB;
                texture.SampleDesc.Count = 1;
                texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                Check(device_->CreateTexture2D(&texture, nullptr, &pool.textures[array]), L"Create bounded BC7 mip pool");
                Check(device_->CreateShaderResourceView(pool.textures[array].Get(), nullptr, &pool.views[array]), L"Create BC7 mip pool view");
            }
        }
        D3D11_SAMPLER_DESC sample{};
        sample.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sample.AddressU = sample.AddressV = sample.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sample.MaxLOD = D3D11_FLOAT32_MAX;
        Check(device_->CreateSamplerState(&sample, &sampler_), L"Create photograph sampler");
        sample.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        Check(device_->CreateSamplerState(&sample, &pointSampler_), L"Create pixel-mosaic sampler");
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        Check(device_->CreateRasterizerState(&raster, &rasterizer_), L"Create billboard rasterizer");
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = TRUE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depth.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
        Check(device_->CreateDepthStencilState(&depth, &depthState_), L"Create depth state");
        depth.DepthEnable = FALSE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        Check(device_->CreateDepthStencilState(&depth, &noDepth_), L"Create compositing depth state");
        depth.DepthEnable = TRUE;
        Check(device_->CreateDepthStencilState(&depth, &lineDepth_), L"Create read-only line depth state");
        D3D11_BLEND_DESC blend{};
        blend.IndependentBlendEnable = TRUE;
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        blend.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        blend.RenderTarget[2].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        Check(device_->CreateBlendState(&blend, &tileBlend_), L"Create rounded-photo coverage blend");
        vs.Reset();
        messages.Reset();
        Check(CompileShaderBytes(GlassShader, strlen(GlassShader), "Nebula glass shader", nullptr, nullptr,
              "FullVS", "vs_5_0", flags, 0, &vs, &messages), L"Compile glass fullscreen vertex shader");
        Check(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &fullscreenShader_),
              L"Create fullscreen vertex shader");
        const char* entries[]{"DownsamplePS", "BlurPS", "CompositePS"};
        ComPtr<ID3D11PixelShader>* targets[]{&downsampleShader_, &blurShader_, &compositeShader_};
        for (int i = 0; i < 3; ++i) {
            ps.Reset(); messages.Reset();
            Check(CompileShaderBytes(GlassShader, strlen(GlassShader), "Nebula glass shader", nullptr, nullptr,
                  entries[i], "ps_5_0", flags, 0, &ps, &messages), L"Compile backdrop blur/composite shader");
            Check(device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, targets[i]->GetAddressOf()),
                  L"Create backdrop blur/composite shader");
        }
        const char* depthEntries[]{"DownPS", "BlurPS", "ResolvePS"};
        ComPtr<ID3D11PixelShader>* depthShaders[]{&depthDown_, &depthBlur_, &depthResolve_};
        for (int i = 0; i < 3; ++i) {
            ps.Reset(); messages.Reset();
            Check(CompileShaderBytes(DepthShader, strlen(DepthShader), "Nebula protected depth", nullptr, nullptr,
                depthEntries[i], "ps_5_0", flags, 0, &ps, &messages), L"Compile protected depth shader");
            Check(device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, depthShaders[i]->GetAddressOf()),
                L"Create protected depth shader");
        }
        for (auto& timer : gpuTimers_) {
            D3D11_QUERY_DESC query{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            Check(device_->CreateQuery(&query, &timer.disjoint), L"Create GPU disjoint timing query");
            query.Query = D3D11_QUERY_TIMESTAMP;
            Check(device_->CreateQuery(&query, &timer.start), L"Create GPU start timestamp");
            Check(device_->CreateQuery(&query, &timer.end), L"Create GPU end timestamp");
        }
        D3D11_QUERY_DESC originalQuery{D3D11_QUERY_EVENT, 0};
        Check(device_->CreateQuery(&originalQuery, &originalRetire_), L"Create focused-original retirement fence");
        ResizeGraphics();
    }

    void EnsureInstanceBuffers(size_t count) {
        size_t capacity = std::max<size_t>(count, 256);
        if (capacity == instanceCapacity_) return;
        ID3D11Buffer* empty = nullptr;
        UINT zero = 0;
        context_->IASetVertexBuffers(0, 1, &empty, &zero, &zero);
        ID3D11ShaderResourceView* emptyViews[]{nullptr, nullptr};
        context_->VSSetShaderResources(20, 2, emptyViews);
        instanceView_.Reset(); orderView_.Reset(); orderBuffer_.Reset();
        for (auto& buffer : instanceBuffers_) buffer.Reset();
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(capacity * sizeof(Instance));
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = sizeof(Instance);
        for (auto& buffer : instanceBuffers_)
            Check(device_->CreateBuffer(&desc, nullptr, &buffer), L"Create scene-sized instance ring");
        Check(device_->CreateShaderResourceView(instanceBuffers_[0].Get(), nullptr, &instanceView_), L"Create compact instance view");
        desc.ByteWidth = static_cast<UINT>(capacity * sizeof(uint32_t));
        desc.StructureByteStride = sizeof(uint32_t);
        Check(device_->CreateBuffer(&desc, nullptr, &orderBuffer_), L"Create compact draw-order stream");
        Check(device_->CreateShaderResourceView(orderBuffer_.Get(), nullptr, &orderView_), L"Create draw-order view");
        instanceCapacity_ = capacity;
        instanceShadow_.assign(capacity, {});
        instanceOrder_.resize(capacity); orderScratch_.resize(capacity); depthKeys_.resize(capacity);
        orderShadow_.assign(capacity, UINT32_MAX);
    }

    void EnsureMosaics(size_t count) {
        if (mosaicCapacity_ == count) return;
        ID3D11ShaderResourceView* empty[7]{};
        context_->PSSetShaderResources(0, 7, empty);
        for (auto& view : mosaicViews_) view.Reset();
        for (auto& buffer : mosaicBuffers_) buffer.Reset();
        for (size_t chunk = 0; chunk < MosaicChunks && chunk * MosaicChunkItems < count; ++chunk) {
            size_t records = std::min(MosaicChunkItems, count - chunk * MosaicChunkItems);
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = static_cast<UINT>(records * ResidentMosaicBytes);
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            Check(device_->CreateBuffer(&desc, nullptr, &mosaicBuffers_[chunk]), L"Create scene-resident mosaic chunk");
            D3D11_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R32_TYPELESS;
            view.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
            view.BufferEx.NumElements = desc.ByteWidth / 4;
            view.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
            Check(device_->CreateShaderResourceView(mosaicBuffers_[chunk].Get(), &view, &mosaicViews_[chunk]), L"Create raw mosaic chunk view");
        }
        mosaicCapacity_ = count;
    }

    void EnsureArt() {
        if (!artRequested_ || items_.empty() || artCapacity_ == items_.size()) return;
        ID3D11ShaderResourceView* empty = nullptr;
        context_->PSSetShaderResources(11, 1, &empty);
        artView_.Reset(); artBuffer_.Reset();
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(items_.size() * ArtBytesPerSlice);
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        Check(device_->CreateBuffer(&desc, nullptr, &artBuffer_), L"Create lazy Pixels art buffer");
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS; view.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        view.BufferEx.NumElements = desc.ByteWidth / 4; view.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        Check(device_->CreateShaderResourceView(artBuffer_.Get(), &view, &artView_), L"Create Pixels art view");
        artCapacity_ = items_.size();
    }

    void ResizeGraphics() {
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        ID3D11ShaderResourceView* empty[]{nullptr, nullptr};
        context_->PSSetShaderResources(0, 2, empty);
        sceneTarget_ = {}; blurA_ = {}; blurB_ = {};
        linearDepth_ = {}; protectionTarget_ = {}; dofHalfA_ = {}; dofHalfB_ = {}; dofOutput_ = {};
        targetView_.Reset();
        depthView_.Reset();
        Check(swapChain_->ResizeBuffers(2, std::max(size_.width, 1), std::max(size_.height, 1),
              DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT), L"ResizeBuffers");
        DXGI_MATRIX_3X2_F transform{1 / size_.scaleX, 0, 0, 1 / size_.scaleY, 0, 0};
        Check(swapChain_->SetMatrixTransform(&transform), L"SetMatrixTransform (physical pixels to DIPs)");
        ComPtr<ID3D11Texture2D> backBuffer;
        Check(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)), L"Get swap-chain back buffer");
        D3D11_RENDER_TARGET_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        Check(device_->CreateRenderTargetView(backBuffer.Get(), &view, &targetView_), L"Create sRGB render target");
        D3D11_TEXTURE2D_DESC depth{};
        depth.Width = std::max(size_.width, 1);
        depth.Height = std::max(size_.height, 1);
        depth.MipLevels = depth.ArraySize = 1;
        depth.Format = DXGI_FORMAT_D32_FLOAT;
        depth.SampleDesc.Count = 1;
        depth.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ComPtr<ID3D11Texture2D> depthTexture;
        Check(device_->CreateTexture2D(&depth, nullptr, &depthTexture), L"Create depth texture");
        Check(device_->CreateDepthStencilView(depthTexture.Get(), nullptr, &depthView_), L"Create depth view");
        for (auto& chunk : chunks_) chunk.grid.Reserve(chunk.end - chunk.begin, size_.width, size_.height);
        {
            std::lock_guard lock(statusMutex_);
            for (auto& picks : publishedPicks_) {
                picks.grid.Reserve(picks.hits.capacity(), size_.width, size_.height);
                picks.grid.Build(picks.hits);
            }
        }
    }

    ColorTarget CreateColorTarget(UINT width, UINT height, DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        ColorTarget result;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        Check(device_->CreateTexture2D(&desc, nullptr, &result.texture), L"Create glass offscreen texture");
        Check(device_->CreateRenderTargetView(result.texture.Get(), nullptr, &result.target), L"Create glass render target");
        Check(device_->CreateShaderResourceView(result.texture.Get(), nullptr, &result.view), L"Create glass shader view");
        return result;
    }

    bool VisibleGlass() const {
        float shadow = nebula_glass::ShadowRadiusDip * std::max(size_.scaleX, size_.scaleY);
        for (const auto& rect : glass_)
            if (rect.opacity > 0 && rect.x - shadow < size_.width && rect.y - shadow < size_.height &&
                rect.x + rect.width + shadow > 0 && rect.y + rect.height + shadow > 0)
                return true;
        return false;
    }

    void EnsureGlassTargets() {
        if (!sceneTarget_.texture) sceneTarget_ = CreateColorTarget(size_.width, size_.height);
        if (blurA_.texture) return;
        blurWidth_ = std::max(1u, (static_cast<UINT>(size_.width) + nebula_glass::Downsample - 1) / nebula_glass::Downsample);
        blurHeight_ = std::max(1u, (static_cast<UINT>(size_.height) + nebula_glass::Downsample - 1) / nebula_glass::Downsample);
        blurA_ = CreateColorTarget(blurWidth_, blurHeight_);
        blurB_ = CreateColorTarget(blurWidth_, blurHeight_);
    }

    void GlassPass(ID3D11RenderTargetView* output, UINT width, UINT height, ID3D11PixelShader* shader,
                   ID3D11ShaderResourceView* source, ID3D11ShaderResourceView* second, const GlassConstants& constants) {
        ID3D11ShaderResourceView* empty[]{nullptr, nullptr};
        context_->PSSetShaderResources(0, 2, empty);
        context_->OMSetRenderTargets(1, &output, nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context_->Map(glassConstants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), L"Map glass constants");
        memcpy(mapped.pData, &constants, sizeof(constants));
        context_->Unmap(glassConstants_.Get(), 0);
        context_->PSSetConstantBuffers(0, 1, glassConstants_.GetAddressOf());
        D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
        context_->RSSetViewports(1, &viewport);
        context_->PSSetShader(shader, nullptr, 0);
        ID3D11ShaderResourceView* sources[]{source, second};
        context_->PSSetShaderResources(0, 2, sources);
        context_->Draw(3, 0);
    }

    void EnsureDepthTargets() {
        if (!sceneTarget_.texture) sceneTarget_ = CreateColorTarget(size_.width, size_.height);
        if (linearDepth_.texture) return;
        linearDepth_ = CreateColorTarget(size_.width, size_.height, DXGI_FORMAT_R32_FLOAT);
        protectionTarget_ = CreateColorTarget(size_.width, size_.height, DXGI_FORMAT_R8_UNORM);
        dofHalfA_ = CreateColorTarget(std::max(1, (size_.width + 1) / 2), std::max(1, (size_.height + 1) / 2));
        dofHalfB_ = CreateColorTarget(std::max(1, (size_.width + 1) / 2), std::max(1, (size_.height + 1) / 2));
        dofOutput_ = CreateColorTarget(size_.width, size_.height);
    }

    void DepthPass(ColorTarget& output, UINT width, UINT height, ID3D11PixelShader* shader,
                   ID3D11ShaderResourceView* input, ID3D11ShaderResourceView* blur, float x, float y) {
        ID3D11ShaderResourceView* empty[4]{};
        context_->PSSetShaderResources(0, 4, empty);
        context_->OMSetRenderTargets(1, output.target.GetAddressOf(), nullptr);
        XMFLOAT4 values[]{ {std::max(lensDistance_, 0.01f), 18, cameraAnimating_ ? 4.0f : 6.0f, 0},
                           {1.0f / size_.width, 1.0f / size_.height, x, y} };
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context_->Map(depthLens_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), L"Map DoF lens");
        memcpy(mapped.pData, values, sizeof(values));
        context_->Unmap(depthLens_.Get(), 0);
        context_->PSSetConstantBuffers(0, 1, depthLens_.GetAddressOf());
        D3D11_VIEWPORT viewport{0,0,static_cast<float>(width),static_cast<float>(height),0,1};
        context_->RSSetViewports(1, &viewport);
        context_->PSSetShader(shader, nullptr, 0);
        ID3D11ShaderResourceView* sources[]{input, linearDepth_.view.Get(), blur, protectionTarget_.view.Get()};
        context_->PSSetShaderResources(0, 4, sources);
        context_->Draw(3, 0);
    }

    void CompositeDepth() {
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(fullscreenShader_.Get(), nullptr, 0);
        context_->OMSetDepthStencilState(noDepth_.Get(), 0);
        context_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
        UINT halfWidth = std::max(1, (size_.width + 1) / 2), halfHeight = std::max(1, (size_.height + 1) / 2);
        DepthPass(dofHalfA_, halfWidth, halfHeight, depthDown_.Get(), sceneTarget_.view.Get(), nullptr, 0, 0);
        DepthPass(dofHalfB_, halfWidth, halfHeight, depthBlur_.Get(), dofHalfA_.view.Get(), nullptr, 1, 0);
        DepthPass(dofHalfA_, halfWidth, halfHeight, depthBlur_.Get(), dofHalfB_.view.Get(), nullptr, 0, 1);
        DepthPass(dofOutput_, size_.width, size_.height, depthResolve_.Get(), sceneTarget_.view.Get(), dofHalfA_.view.Get(), 0, 0);
        ID3D11ShaderResourceView* empty[4]{};
        context_->PSSetShaderResources(0, 4, empty);
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        std::swap(sceneTarget_, dofOutput_);
    }

    void CompositeGlass() {
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(fullscreenShader_.Get(), nullptr, 0);
        context_->OMSetDepthStencilState(noDepth_.Get(), 0);
        context_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
        GlassConstants constants{};
        constants.outputInfo = {static_cast<float>(size_.width), static_cast<float>(size_.height),
                                static_cast<float>(glass_.size()), light_ ? 1.0f : 0.0f};
        constants.displayScale = {size_.scaleX, size_.scaleY, nebula_glass::ShadowRadiusDip, nebula_glass::ShadowOffsetDip};
        for (size_t i = 0; i < glass_.size(); ++i) {
            const auto& rect = glass_[i];
            constants.bounds[i] = {rect.x, rect.y, rect.width, rect.height};
            constants.material[i] = {rect.radius, rect.tint, rect.opacity, 0};
        }
        constants.blurInfo = {1.0f / size_.width, 1.0f / size_.height, 0, 0};
        GlassPass(blurA_.target.Get(), blurWidth_, blurHeight_, downsampleShader_.Get(), sceneTarget_.view.Get(), nullptr, constants);
        for (int iteration = 0; iteration < nebula_glass::Iterations; ++iteration) {
            constants.blurInfo = {1.0f / blurWidth_, 1.0f / blurHeight_,
                                 size_.scaleX * nebula_glass::KernelStepPerScale, 0};
            GlassPass(blurB_.target.Get(), blurWidth_, blurHeight_, blurShader_.Get(), blurA_.view.Get(), nullptr, constants);
            constants.blurInfo.z = 0;
            constants.blurInfo.w = size_.scaleY * nebula_glass::KernelStepPerScale;
            GlassPass(blurA_.target.Get(), blurWidth_, blurHeight_, blurShader_.Get(), blurB_.view.Get(), nullptr, constants);
        }
        GlassPass(targetView_.Get(), size_.width, size_.height, compositeShader_.Get(), sceneTarget_.view.Get(), blurA_.view.Get(), constants);
        ID3D11ShaderResourceView* empty[]{nullptr, nullptr};
        context_->PSSetShaderResources(0, 2, empty);
    }

    void ConfigureFrameCpuSets() {
        ULONG bytes = 0;
        if (!GetSystemCpuSetInformation(nullptr, 0, &bytes, GetCurrentProcess(), 0) && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            Error(HRESULT_FROM_WIN32(GetLastError()), L"Query frame CPU topology");
            return;
        }
        if (!bytes) return;
        std::vector<SYSTEM_CPU_SET_INFORMATION> storage((bytes + sizeof(SYSTEM_CPU_SET_INFORMATION) - 1) / sizeof(SYSTEM_CPU_SET_INFORMATION));
        if (!GetSystemCpuSetInformation(storage.data(), static_cast<ULONG>(storage.size() * sizeof(storage[0])), &bytes, GetCurrentProcess(), 0)) {
            Error(HRESULT_FROM_WIN32(GetLastError()), L"Read frame CPU topology");
            return;
        }
        std::vector<std::pair<ULONG, BYTE>> available;
        BYTE slowest = 255, fastest = 0;
        for (size_t offset = 0; offset < bytes;) {
            auto* entry = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(reinterpret_cast<const unsigned char*>(storage.data()) + offset);
            if (bytes - offset < sizeof(*entry) || entry->Size < sizeof(*entry) || entry->Size > bytes - offset) {
                Error(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), L"Invalid frame CPU topology");
                return;
            }
            if (entry->Type == CpuSetInformation && (!entry->CpuSet.Allocated || entry->CpuSet.AllocatedToTargetProcess)) {
                slowest = std::min(slowest, entry->CpuSet.EfficiencyClass);
                fastest = std::max(fastest, entry->CpuSet.EfficiencyClass);
                available.emplace_back(entry->CpuSet.Id, entry->CpuSet.EfficiencyClass);
            }
            offset += entry->Size;
        }
        if (slowest == fastest || available.empty()) return;
        for (auto [id, efficiency] : available) if (efficiency == fastest) frameCpuSets_.push_back(id);
        framePartitions_ = std::min(framePartitions_, frameCpuSets_.size());
        chunks_.resize(framePartitions_); publishedPicks_.resize(framePartitions_); frameWorkers_.resize(framePartitions_ - 1);
    }

    void SelectFrameCpuSets() {
        if (!frameCpuSets_.empty() && !SetThreadSelectedCpuSets(GetCurrentThread(), frameCpuSets_.data(), static_cast<ULONG>(frameCpuSets_.size())))
            Error(HRESULT_FROM_WIN32(GetLastError()), L"Select frame performance CPU sets");
    }

    void Run() noexcept {
        HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        try {
            Check(com, L"Initialize render-thread COM");
            if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL))
                throw HrFailure{HRESULT_FROM_WIN32(GetLastError()), L"Set interactive render-thread priority"};
            ConfigureFrameCpuSets();
            InitializeGraphics();
            originalLoader_ = std::make_unique<nebula_original::Loader>([this] {
                std::lock_guard lock(commandMutex_);
                wake_.notify_one();
            });
            for (size_t worker = 0; worker < frameWorkers_.size(); ++worker)
                frameWorkers_[worker] = std::thread([this, worker] { FrameWorker(worker + 1); });
            for (auto& worker : workers_) worker = std::thread([this] { DecodeWorker(); });
            SelectFrameCpuSets();
            {
                std::lock_guard lock(commandMutex_);
                initialized_ = true;
                wake_.notify_all();
            }
            while (!stop_.load()) {
                Commands pending;
                {
                    std::unique_lock lock(commandMutex_);
                    auto workReady = [this] {
                        bool visible = size_.width > 0 && size_.height > 0;
                        return stop_.load() || (attached_ && (commands_.dirty ||
                            (visible && (animating_ || cameraAnimating_ || effectsAnimating_ || readyResults_.load() > 0 ||
                             pendingOriginal_.has_value() || (originalLoader_ && originalLoader_->Ready())))));
                    };
                    if (!workReady()) {
                        // Publish scheduler quiescence, not the last active-frame average.
                        // commandMutex_ orders this with input and decode-arrival wakeups.
                        std::lock_guard statusLock(statusMutex_);
                        fps_ = 0;
                        stats_.fps = 0;
                        resumedFromIdle_ = true;
                    }
                    wake_.wait(lock, workReady);
                    if (stop_.load()) break;
                    pending = std::move(commands_);
                    commands_ = {};
                    Size nextSize = pending.size.value_or(size_);
                    lock.unlock();
                    DWORD ready = WAIT_TIMEOUT;
                    if (nextSize.width <= 0 || nextSize.height <= 0) ready = WAIT_OBJECT_0;
                    while (!stop_.load() && ready == WAIT_TIMEOUT) ready = WaitForSingleObject(frameReady_, 100);
                    if (stop_.load()) break;
                    if (ready != WAIT_OBJECT_0) throw HrFailure{ready == WAIT_FAILED ? HRESULT_FROM_WIN32(GetLastError()) : E_FAIL,
                                                               L"Wait for composition frame latency"};
                }
                const auto now = Clock::now();
                frameStarted_ = now;
                Apply(pending, now);
                Animate(now);
                if (size_.width > 0 && size_.height > 0) RenderFrame();
                else {
                    SnapCamera();
                    AnimateLayout(animationStart_ + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(morphDuration_)));
                    effectsAnimating_ = false;
                    PublishStats(false);
                }
            }
        } catch (const HrFailure& error) {
            HRESULT hr = error.hr;
            if (device_ && (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)) hr = device_->GetDeviceRemovedReason();
            Error(FAILED(hr) ? hr : error.hr, error.operation, true);
        } catch (const std::bad_alloc&) {
            Error(E_OUTOFMEMORY, L"Renderer allocation", true);
        } catch (...) {
            Error(E_FAIL, L"Unexpected render-thread failure", true);
        }
        stop_.store(true);
        {
            std::lock_guard lock(frameMutex_);
            stopFrameWorkers_ = true;
            frameWake_.notify_all();
        }
        for (auto& worker : frameWorkers_) if (worker.joinable()) worker.join();
        {
            std::lock_guard lock(commandMutex_);
            initialized_ = true;
            wake_.notify_all();
        }
        {
            std::lock_guard lock(decodeMutex_);
            decodeWake_.notify_all();
            resultSpace_.notify_all();
        }
        if (context_) { context_->ClearState(); context_->Flush(); }
        if (SUCCEEDED(com)) CoUninitialize();
    }

    static Decoded Decode(IWICImagingFactory* factory, const Request& request, size_t index) {
        Decoded result;
        result.generation = request.generation;
        result.index = index;
        result.tier = request.tier;
        try {
            const auto& path = (*request.paths)[index];
            if (path.empty()) throw HrFailure{HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), L"Empty photograph path"};
            uint32_t edge = request.tier == 1 ? 256 : MipEdges[request.tier - 2];
            std::optional<nebula_cache::ContentKey> key;
            try { key = nebula_cache::SourceKey(path, request.versions ? (*request.versions)[index] : 0); }
            catch (const nebula_cache::Failure& failure) { result.cacheError = failure.hr; }
            if (request.cache && key) {
                nebula_cache::Payload cached;
                HRESULT found = request.cache->Read(*key, request.tier == 1 ? 0 : edge, cached);
                if (found == S_OK) {
                    result.aspect = cached.aspect;
                    if (request.tier == 1) result.mosaicPixels = std::move(cached.bytes);
                    else {
                        result.pixels = std::move(cached.bytes); result.mipOffsets = cached.offsets; result.mipLevels = cached.levels;
                    }
                    result.cacheError = request.cache->Warning();
                    return result;
                }
                if (FAILED(found)) result.cacheError = found;
            }
            auto decoded = nebula_decode::Decode(factory, path, edge, request.tier == 5);
            result.aspect = decoded.aspect;
            result.pixels = std::move(decoded.pixels);
            result.uv = {decoded.uv[0], decoded.uv[1], decoded.uv[2], decoded.uv[3]};
            nebula_cache::Payload payload;
            if (request.tier == 1) {
                result.mosaicPixels = BuildMosaicAverages(result.pixels, edge, decoded.x, decoded.y, decoded.width, decoded.height, result.aspect);
                payload.bytes = result.mosaicPixels;
                payload.aspect = result.aspect;
            } else {
                payload = nebula_cache::Encode(result.pixels, edge, result.aspect);
                result.mipOffsets = payload.offsets;
                result.mipLevels = payload.levels;
                result.pixels = payload.bytes;
            }
            payload.sourceWidth = decoded.sourceWidth; payload.sourceHeight = decoded.sourceHeight;
            if (request.cache && key) {
                try {
                    auto verified = nebula_cache::SourceKey(path, request.versions ? (*request.versions)[index] : 0);
                    if (verified != *key) result.cacheError = HRESULT_FROM_WIN32(ERROR_RETRY);
                    else {
                        HRESULT saved = request.cache->Write(*key, payload);
                        if (FAILED(saved)) result.cacheError = saved;
                    }
                } catch (const nebula_cache::Failure& failure) { result.cacheError = failure.hr; }
            }
        } catch (const HrFailure& error) { result.hr = error.hr; result.operation = error.operation; result.pixels.clear(); }
          catch (const nebula_original::Failure& error) { result.hr = error.hr; result.operation = error.operation; result.pixels.clear(); }
        return result;
    }

    void DecodeWorker() noexcept {
        HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        try {
            Check(com, L"Initialize WIC-worker COM");
            if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL))
                throw HrFailure{HRESULT_FROM_WIN32(GetLastError()), L"Set background decoder priority"};
            ComPtr<IWICImagingFactory> factory;
            Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                  IID_PPV_ARGS(&factory)), L"Create WIC decoder factory");
            std::unordered_map<std::wstring, MosaicRecord> mosaicCache;
            std::array<std::wstring, 64> cacheOrder;
            size_t cacheNext = 0;
            uint64_t cacheGeneration = 0;
            unsigned photoBurst = 0;
            for (;;) {
                Request request;
                {
                    std::unique_lock lock(decodeMutex_);
                    decodeWake_.wait(lock, [this] { return stop_.load() || !requests_.empty() || !mosaicRequests_.empty(); });
                    if (stop_.load()) break;
                    if (!requests_.empty() && (mosaicRequests_.empty() || photoBurst < 32)) {
                        request = std::move(requests_.front());
                        requests_.pop_front();
                        ++photoBurst;
                    } else {
                        request = std::move(mosaicRequests_.front());
                        mosaicRequests_.pop_front();
                        photoBurst = 0;
                    }
                }
                if (request.generation != generation_.load()) continue;
                Decoded result;
                if (request.tier == 1) {
                    if (cacheGeneration != request.generation) {
                        mosaicCache.clear();
                        for (auto& path : cacheOrder) path.clear();
                        cacheGeneration = request.generation;
                        cacheNext = 0;
                    }
                    result.generation = request.generation;
                    result.tier = 1;
                    result.mosaics.reserve(request.count);
                    for (size_t i = request.index; i < request.index + request.count; ++i) {
                        if (stop_.load() || request.generation != generation_.load()) break;
                        const auto& path = (*request.paths)[i];
                        auto cached = mosaicCache.find(path);
                        MosaicRecord record;
                        if (cached != mosaicCache.end()) record = cached->second;
                        else {
                            auto decoded = Decode(factory.Get(), request, i);
                            record.hr = decoded.hr;
                            record.cacheError = decoded.cacheError;
                            record.operation = decoded.operation;
                            record.aspect = decoded.aspect;
                            if (SUCCEEDED(record.hr)) std::copy_n(decoded.mosaicPixels.data(), MosaicBytesPerSlice, record.pixels.data());
                            if (mosaicCache.size() == cacheOrder.size()) mosaicCache.erase(cacheOrder[cacheNext]);
                            cacheOrder[cacheNext] = path;
                            cacheNext = (cacheNext + 1) % cacheOrder.size();
                            mosaicCache.insert_or_assign(path, record);
                        }
                        record.index = i;
                        result.mosaics.push_back(std::move(record));
                    }
                } else result = Decode(factory.Get(), request, request.index);
                std::unique_lock lock(decodeMutex_);
                resultSpace_.wait(lock, [this, &result] {
                    return stop_.load() || result.generation != generation_.load() || !results_.full();
                });
                if (stop_.load()) break;
                if (result.generation != generation_.load()) continue;
                results_.push_back(std::move(result));
                readyResults_.store(results_.size());
                lock.unlock();
                {
                    std::lock_guard commandLock(commandMutex_);
                    wake_.notify_one();
                }
            }
        } catch (const HrFailure& error) {
            Error(error.hr, error.operation, true);
            stop_.store(true);
        } catch (const std::bad_alloc&) {
            Error(E_OUTOFMEMORY, L"WIC worker allocation", true);
            stop_.store(true);
        } catch (...) {
            Error(E_FAIL, L"Unexpected WIC-worker failure", true);
            stop_.store(true);
        }
        {
            std::lock_guard lock(commandMutex_);
            wake_.notify_all();
        }
        {
            std::lock_guard lock(decodeMutex_);
            decodeWake_.notify_all();
            resultSpace_.notify_all();
        }
        if (SUCCEEDED(com)) CoUninitialize();
    }

    void Apply(Commands& c, Clock::time_point now) {
        if (!c.scene && (c.positions || c.results || c.clearResults)) AnimateLayout(now);
        if (c.size) {
            size_ = *c.size;
            if (!size_.width || !size_.height) for (auto& item : items_) item.onScreen = false;
            if (size_.width > 0 && size_.height > 0) ResizeGraphics();
        }
        if (c.reducedMotion) reducedMotion_ = *c.reducedMotion != 0;
        if (c.glass) {
            glass_ = std::move(*c.glass);
            if (glass_.empty()) {
                if (!focusDof_ || !originalView_) sceneTarget_ = {};
                blurA_ = {}; blurB_ = {};
            }
        }
        if (c.scene) {
            items_ = std::move(c.scene->items);
            cold_ = std::move(c.scene->cold);
            sceneResourcesDirty_ = true;
            paths_ = std::move(c.scene->paths);
            originals_ = std::move(c.scene->originals);
            contentVersions_ = std::move(c.scene->versions);
            originalLoader_->Cancel();
            requestedOriginal_ = -1;
            pendingOriginal_.reset();
            originalUploadTexture_.Reset(); originalUploadRow_ = 0;
            focusState_ = {sizeof(NbFocusStats), -1};
            originalTexture_.Reset(); originalView_.Reset();
            ++generation_;
            {
                std::lock_guard lock(decodeMutex_);
                requests_.clear();
                mosaicRequests_.clear();
                results_.clear();
                readyResults_ = 0;
            }
            resultSpace_.notify_all();
            for (auto& pool : imagePools_) {
                pool.unusedSlot = 0;
                pool.reuseFrame = UINT64_MAX;
                pool.reusable.clear();
                for (auto& slot : pool.slots) slot = {};
            }
            size_t libraryCount = paths_->size();
            uint64_t libraryKey = nebula_cache::Hash(&libraryCount, sizeof(libraryCount));
            for (size_t i = 0; i < paths_->size(); ++i) {
                libraryKey = nebula_cache::Hash((*paths_)[i].data(), (*paths_)[i].size() * sizeof(wchar_t), libraryKey);
                libraryKey = nebula_cache::Hash(&(*contentVersions_)[i], sizeof(uint64_t), libraryKey);
            }
            try {
                // Old-generation workers can still hold this library's writable pack.
                if (!cache_ || !cache_->CanReuse(libraryKey)) cache_ = std::make_shared<nebula_cache::Store>(libraryKey);
            }
            catch (const nebula_cache::Failure& failure) { cache_.reset(); Error(failure.hr, L"Open render-cache root"); }
            nextMosaic_ = 0;
            nextArt_ = 0;
            artRequested_ = pixels_ == 5;
            artCapacity_ = 0;
            artView_.Reset(); artBuffer_.Reset();
            pendingJobs_ = mosaicJobs_ = mosaicReady_ = mosaicDone_ = failed_ = 0;
            frameResident_ = 0;
            caps_ = nebula_lod::Floors;
            sizeCap0_ = nebula_lod::Floors[0];
            sizeEdges_.resize(items_.size());
            lineVertices_.reserve(nebula_highlight::MaxLines * 2);
            for (size_t i = 0; i < chunks_.size(); ++i) {
                auto& chunk = chunks_[i];
                chunk.begin = items_.size() * i / chunks_.size();
                chunk.end = items_.size() * (i + 1) / chunks_.size();
                size_t capacity = chunk.end - chunk.begin;
                chunk.tiles.reserve(capacity); chunk.hits.reserve(capacity);
                chunk.grid.Reserve(capacity, size_.width, size_.height);
            }
            {
                std::lock_guard lock(statusMutex_);
                for (size_t i = 0; i < publishedPicks_.size(); ++i) {
                    auto& picks = publishedPicks_[i];
                    picks.hits.clear(); picks.hits.reserve(chunks_[i].end - chunks_[i].begin);
                    picks.grid.Reserve(chunks_[i].end - chunks_[i].begin, size_.width, size_.height);
                    picks.grid.Build(picks.hits);
                }
            }
            selected_ = -1;
            hovered_ = -1;
            animating_ = false;
            resultsActive_ = false;
            selectionMix_ = 0;
            highlightMix_ = 0;
            highlightActive_ = false;
            lines_.clear();
            searchCamera_.reset();
            history_.clear();
            itemIndex_.clear();
            itemIndex_.reserve(items_.size());
            for (size_t i = 0; i < items_.size(); ++i) itemIndex_.emplace(items_[i].id, i);
        }
        bool retarget = c.positions.has_value() || c.results.has_value() || (c.clearResults && resultsActive_);
        if (retarget) for (auto& item : items_) {
            item.cold->from = item.position;
            item.cold->alphaFrom = item.alpha;
            item.cold->scaleFrom = item.scale;
            item.cold->overlayFrom = item.overlay;
        }
        if (c.positions && c.positions->size() == items_.size()) {
            for (size_t i = 0; i < items_.size(); ++i) {
                items_[i].cold->base = (*c.positions)[i];
                if (!resultsActive_ || !items_[i].matched) items_[i].cold->target = items_[i].cold->base;
            }
        }
        if (c.results) {
            if (!resultsActive_) {
                searchCamera_ = desired_;
            }
            resultsActive_ = true;
            for (auto& item : items_) {
                item.matched = false;
                item.cold->target = item.cold->base;
                item.cold->alphaTarget = 0.08f;
                item.cold->scaleTarget = 0.35f;
                item.cold->overlayTarget = 0;
            }
            double best = -DBL_MAX, worst = DBL_MAX;
            for (const auto& result : *c.results) {
                best = std::max(best, static_cast<double>(result.weight));
                worst = std::min(worst, static_cast<double>(result.weight));
            }
            for (const auto& result : *c.results) {
                auto found = itemIndex_.find(result.id);
                if (found == itemIndex_.end()) continue;
                auto& item = items_[found->second];
                float confidence = best == worst ? 0.0f : static_cast<float>((result.weight - worst) / (best - worst));
                item.matched = true;
                item.cold->target = {result.x, result.y, result.z};
                item.cold->alphaTarget = 1;
                item.cold->scaleTarget = 0.85f + 0.75f * confidence;
                item.cold->overlayTarget = 1;
            }
        } else if (c.clearResults && resultsActive_) {
            resultsActive_ = false;
            for (auto& item : items_) {
                item.matched = false;
                item.cold->target = item.cold->base;
                item.cold->alphaTarget = item.cold->scaleTarget = 1;
                item.cold->overlayTarget = 0;
            }
            if (searchCamera_) desired_ = *searchCamera_;
            searchCamera_.reset();
        }
        if (retarget) {
            animationStart_ = now;
            morphDuration_ = c.results || c.clearResults ? 0.28f : 0.5f;
            animating_ = !items_.empty();
        }
        if (c.filter && c.filter->size() == items_.size())
            for (size_t i = 0; i < items_.size(); ++i) items_[i].visible = (*c.filter)[i] != 0;
        if (c.highlight) {
            for (auto& item : items_) item.highlightTarget = 0;
            for (const auto& [id, weight] : *c.highlight) items_[itemIndex_.at(id)].highlightTarget = weight;
            highlightActive_ = !c.highlight->empty();
        }
        if (c.lines) lines_ = std::move(*c.lines);
        if (c.selection) selected_ = *c.selection;
        if (c.hover && hovered_ != *c.hover) {
            hovered_ = *c.hover;
            hoverSince_ = now;
        }
        if (c.pixels) {
            pixels_ = *c.pixels;
            if (pixels_ == 5) artRequested_ = true;
        }
        if (c.theme) light_ = *c.theme != 0;
        if (c.depthOptions) { atmosphere_ = c.depthOptions->first != 0; focusDof_ = c.depthOptions->second != 0; }
        if (c.back || c.fit || c.results || c.focus || c.focusSet || c.scene) zoomPending_ = 0;
        if (c.back && !history_.empty()) {
            desired_ = history_.back();
            history_.pop_back();
        }
        if (c.fit) {
            if (!c.scene) PushHistory();
            Fit();
        }
        if (c.results) Fit();
        if (c.focus) FocusPhoto(*c.focus);
        if (c.focusSet) FocusPhotos(*c.focusSet);
        desired_.yaw = std::remainder(desired_.yaw - c.orbitX * 0.006f, XM_2PI);
        desired_.pitch = std::clamp(desired_.pitch + c.orbitY * 0.006f, -1.35f, 1.35f);
        // Wheel zoom is queued in log-distance and released by Animate(), not applied as a step.
        zoomPending_ = std::clamp(zoomPending_ - c.zoom / 120.0f * 0.16f, -30.0f, 30.0f);
        float unitsPerPixel = 2 * desired_.distance * std::tan(Fov / 2) / std::max(size_.height, 1);
        XMMATRIX inverse = XMMatrixInverse(nullptr, CameraView(desired_));
        XMVECTOR movement = -c.panX * unitsPerPixel * size_.scaleX * inverse.r[0] +
                            c.panY * unitsPerPixel * size_.scaleY * inverse.r[1];
        XMVECTOR moved = XMLoadFloat3(&desired_.target) + movement;
        XMStoreFloat3(&desired_.target, XMVectorClamp(moved, XMVectorReplicate(-2000000), XMVectorReplicate(2000000)));
        if (c.scene || reducedMotion_) SnapCamera();
        if (requestedOriginal_ != selected_) {
            originalLoader_->Cancel();
            pendingOriginal_.reset();
            originalUploadTexture_.Reset(); originalUploadRow_ = 0;
            requestedOriginal_ = selected_;
            requestedUv_ = {0,0,1,1};
            focusState_ = {sizeof(NbFocusStats), selected_};
            originalTexture_.Reset(); originalView_.Reset();
            if (selected_ >= 0 && originals_ && !(*originals_)[itemIndex_.at(static_cast<uint32_t>(selected_))].empty()) {
                size_t index = itemIndex_.at(static_cast<uint32_t>(selected_));
                if (FAILED(items_[index].cold->originalError)) {
                    focusState_.state = 5;
                    focusState_.error = items_[index].cold->originalError;
                } else {
                    focusState_.state = 1; focusState_.pending = 1;
                    originalSerial_ = originalLoader_->RequestImage(generation_, selected_, (*originals_)[index]);
                }
            }
        }
    }

    void Fit() {
        if (items_.empty()) { desired_.target = {}; desired_.distance = 100; return; }
        XMFLOAT3 lo{FLT_MAX, FLT_MAX, FLT_MAX}, hi{-FLT_MAX, -FLT_MAX, -FLT_MAX};
        bool any = false;
        for (const auto& item : items_) if (item.visible && (!resultsActive_ || item.matched)) {
            any = true;
            lo.x = std::min(lo.x, item.cold->target.x); hi.x = std::max(hi.x, item.cold->target.x);
            lo.y = std::min(lo.y, item.cold->target.y); hi.y = std::max(hi.y, item.cold->target.y);
            lo.z = std::min(lo.z, item.cold->target.z); hi.z = std::max(hi.z, item.cold->target.z);
        }
        if (!any) return;
        desired_.target = {(lo.x + hi.x) / 2, (lo.y + hi.y) / 2, (lo.z + hi.z) / 2};
        float aspect = static_cast<float>(std::max(size_.width, 1)) / std::max(size_.height, 1);
        CameraState centered = desired_;
        centered.target = {};
        centered.distance = 1;
        XMMATRIX orientation = CameraView(centered);
        float fitDistance = 0.01f;
        for (const auto& item : items_) if (item.visible && (!resultsActive_ || item.matched)) {
            XMFLOAT3 p{};
            XMStoreFloat3(&p, XMVector3TransformNormal(XMLoadFloat3(&item.cold->target) - XMLoadFloat3(&desired_.target), orientation));
            float lift = item.cold->scaleTarget * (static_cast<int>(item.id) == selected_ ? 1.45f : 1.0f);
            float tileHalf = TileHalfSize * (resultsActive_ ? 1.25f : 1.0f);
            float hx = tileHalf * std::min(item.aspect, 1.0f) * lift;
            float hy = tileHalf * std::min(1 / item.aspect, 1.0f) * lift;
            fitDistance = std::max({fitDistance, (std::abs(p.x) + hx) / (std::tan(Fov / 2) * aspect * 0.70f) - p.z,
                                   (std::abs(p.y) + hy) / (std::tan(Fov / 2) * 0.70f) - p.z, -p.z + NearPlane * 2});
            // Fit must not immediately place ordinary photos inside either safety fade.
            if (static_cast<int>(item.id) != selected_)
                fitDistance = std::max({fitDistance, tileHalf * lift / (std::tan(Fov / 2) * .55f) - p.z, -p.z / .65f});
        }
        desired_.distance = std::clamp(fitDistance, 0.01f, 5000000.0f);
    }

    void PushHistory() {
        if (history_.size() == 32) history_.pop_front();
        history_.push_back(desired_);
    }

    void FocusPhoto(int id) {
        auto item = std::find_if(items_.begin(), items_.end(), [id](const Item& value) { return static_cast<int>(value.id) == id; });
        if (item == items_.end()) return;
        PushHistory();
        desired_.target = item->cold->target;
        float tileHalf = TileHalfSize * (resultsActive_ ? 1.25f : 1.0f);
        float hx = tileHalf * std::min(item->aspect, 1.0f) * item->cold->scaleTarget * 1.45f;
        float hy = tileHalf * std::min(1 / item->aspect, 1.0f) * item->cold->scaleTarget * 1.45f;
        float aspect = static_cast<float>(std::max(size_.width, 1)) / std::max(size_.height, 1);
        desired_.distance = std::clamp(std::max(hy / (std::tan(Fov / 2) * 0.40f),
                                              hx / (std::tan(Fov / 2) * aspect * 0.72f)), 0.01f, 5000000.0f);
        selected_ = id;
    }

    void FocusPhotos(const std::vector<uint32_t>& ids) {
        XMVECTOR sum = XMVectorZero();
        XMVECTOR lo = XMVectorReplicate(FLT_MAX), hi = XMVectorReplicate(-FLT_MAX);
        for (uint32_t id : ids) {
            XMVECTOR position = XMLoadFloat3(&items_[itemIndex_.at(id)].cold->target);
            sum += position;
            lo = XMVectorMin(lo, position);
            hi = XMVectorMax(hi, position);
        }
        PushHistory();
        XMStoreFloat3(&desired_.target, sum / static_cast<float>(ids.size()));
        XMFLOAT3 extents{};
        XMStoreFloat3(&extents, hi - lo);
        float span = std::max({extents.x, extents.y, extents.z});
        desired_.distance = std::clamp(span * 1.4f, nebula_highlight::FocusMin, nebula_highlight::FocusMax);
    }

    static XMMATRIX CameraView(const CameraState& camera) {
        XMVECTOR direction = XMVectorSet(std::sin(camera.yaw) * std::cos(camera.pitch), std::sin(camera.pitch),
                                        -std::cos(camera.yaw) * std::cos(camera.pitch), 0);
        // Keep the direction independent of large world-coordinate subtraction at close zoom.
        return XMMatrixLookToLH(XMLoadFloat3(&camera.target) + direction * camera.distance,
                                -direction, XMVectorSet(0, 1, 0, 0));
    }

    void SnapCamera() {
        yaw_ = desired_.yaw; pitch_ = desired_.pitch; distance_ = desired_.distance; target_ = desired_.target;
        cameraAnimating_ = false;
    }

    void AnimateLayout(Clock::time_point now) {
        if (!animating_) return;
        float t = reducedMotion_ ? 1 : std::clamp(std::chrono::duration<float>(now - animationStart_).count() / morphDuration_, 0.0f, 1.0f);
        float smooth = Smooth(t);
        for (auto& item : items_) {
            XMStoreFloat3(&item.position, XMVectorLerp(XMLoadFloat3(&item.cold->from), XMLoadFloat3(&item.cold->target), smooth));
            item.alpha = item.cold->alphaFrom + (item.cold->alphaTarget - item.cold->alphaFrom) * smooth;
            item.scale = item.cold->scaleFrom + (item.cold->scaleTarget - item.cold->scaleFrom) * smooth;
            item.overlay = item.cold->overlayFrom + (item.cold->overlayTarget - item.cold->overlayFrom) * smooth;
        }
        if (t >= 1) animating_ = false;
    }

    void Animate(Clock::time_point now) {
        float elapsed = std::chrono::duration<float>(now - visualTime_).count();
        frameDelta_ = resumedFromIdle_ ? 1.0f / 60 : std::clamp(elapsed, 0.001f, 0.25f);
        resumedFromIdle_ = false;
        visualTime_ = now;
        float amount = reducedMotion_ ? 1 : 1 - std::exp(-frameDelta_ / 0.095f);
        bool zooming = zoomPending_ != 0;
        if (zooming) desired_.distance = std::clamp(
            desired_.distance * std::exp(nebula_lod::TakeZoom(zoomPending_, frameDelta_, reducedMotion_)), 0.01f, 5000000.0f);
        float dyaw = std::remainder(desired_.yaw - yaw_, XM_2PI);
        float targetDifference = XMVectorGetX(XMVector3Length(XMLoadFloat3(&desired_.target) - XMLoadFloat3(&target_)));
        cameraAnimating_ = zooming || std::abs(dyaw) > 0.0002f || std::abs(desired_.pitch - pitch_) > 0.0002f ||
            std::abs(desired_.distance - distance_) > std::max(0.0001f, desired_.distance * 0.0005f) ||
            targetDifference > TileHalfSize * 0.002f;
        if (!cameraAnimating_ || reducedMotion_) SnapCamera();
        else {
            yaw_ = std::remainder(yaw_ + dyaw * amount, XM_2PI);
            pitch_ += (desired_.pitch - pitch_) * amount;
            distance_ += (desired_.distance - distance_) * amount;
            XMStoreFloat3(&target_, XMVectorLerp(XMLoadFloat3(&target_), XMLoadFloat3(&desired_.target), amount));
        }
        effectsAnimating_ = false;
        effectsAnimating_ |= nebula_highlight::Ease(highlightMix_, highlightActive_ ? 1.0f : 0.0f,
            nebula_highlight::MixStepAt60Hz, frameDelta_, reducedMotion_);
        float selectionStep = reducedMotion_ ? 1 : frameDelta_ / 0.16f;
        effectsAnimating_ |= Approach(selectionMix_, selected_ >= 0 ? 1.0f : 0.0f, selectionStep);
        bool hoverReady = reducedMotion_ || now - hoverSince_ >= std::chrono::milliseconds(100);
        if (hovered_ >= 0 && !hoverReady) effectsAnimating_ = true;
    }

    void UpdateCamera() {
        view_ = CameraView({yaw_, pitch_, distance_, target_});
        projection_ = XMMatrixPerspectiveFovLH(Fov, static_cast<float>(std::max(size_.width, 1)) /
                                              std::max(size_.height, 1), FarPlane, NearPlane);
        projectionPixels_ = {XMVectorGetX(projection_.r[0]) * size_.width * .5f,
                             XMVectorGetY(projection_.r[1]) * size_.height * .5f};
        worldPerPixel_ = {1 / projectionPixels_.x, 1 / projectionPixels_.y};
    }

    float VisualAlpha(const Item& item) const {
        return nebula_highlight::Alpha(item.alpha, Smooth(selectionMix_), Smooth(item.selection), item.highlight, highlightMix_);
    }

    // Depth priority only for photos brought to the camera (gather/overlay). A highlight is shown
    // by its ring, lift and the dimming of everything else, never by drawing over nearer photos.
    float Presentation(const Item& item) const { return item.overlay; }

    void PrepareChunk(size_t which) {
        auto& chunk = chunks_[which];
        auto now = prepareTime_;
        chunk.tiles.clear(); chunk.hits.clear(); chunk.grid.Begin();
        chunk.wantCount = 0; chunk.histogram.fill(0); chunk.lod = {sizeof(NbLodStats)};
        chunk.moving = chunk.originalUsed = false;
        chunk.instanceFirst = SIZE_MAX;
        chunk.instanceLast = 0;
        chunk.submittedDots = 0;
        XMVECTOR selectedPosition = XMVectorZero();
        auto selected = selected_ >= 0 ? itemIndex_.find(static_cast<uint32_t>(selected_)) : itemIndex_.end();
        float px = XMVectorGetX(projection_.r[0]), py = XMVectorGetY(projection_.r[1]);
        float kpx = size_.height * 0.5f * py;
        float tileWidth = resultsActive_ ? 1.25f : 1.0f;
        bool hoverReady = reducedMotion_ || now - hoverSince_ >= std::chrono::milliseconds(100);
        float hoverAmount = reducedMotion_ ? 1 : 1 - std::exp(-frameDelta_ / 0.067f);
        float selectionStep = reducedMotion_ ? 1 : frameDelta_ / 0.16f;
        float layoutT = reducedMotion_ ? 1 : std::clamp(std::chrono::duration<float>(now - animationStart_).count() / morphDuration_, 0.0f, 1.0f);
        float layoutMix = Smooth(layoutT);
        if (selected != itemIndex_.end()) {
            const auto& item = items_[selected->second];
            selectedPosition = animating_
                ? XMVectorLerp(XMLoadFloat3(&item.cold->from), XMLoadFloat3(&item.cold->target), layoutMix)
                : XMLoadFloat3(&item.position);
        }
        float attentionAmount = nebula_lod::Amount(0.18f, frameDelta_);
        for (size_t index = chunk.begin; index < chunk.end; ++index) {
            auto& item = items_[index];
            sizeEdges_[index] = 0;
            if (animating_) {
                XMStoreFloat3(&item.position, XMVectorLerp(XMLoadFloat3(&item.cold->from), XMLoadFloat3(&item.cold->target), layoutMix));
                item.alpha = item.cold->alphaFrom + (item.cold->alphaTarget - item.cold->alphaFrom) * layoutMix;
                item.scale = item.cold->scaleFrom + (item.cold->scaleTarget - item.cold->scaleFrom) * layoutMix;
                item.overlay = item.cold->overlayFrom + (item.cold->overlayTarget - item.cold->overlayFrom) * layoutMix;
            }
            float hover = hoverReady && item.visible && static_cast<int>(item.id) == hovered_ ? 1.0f : 0.0f;
            if (item.hover != hover) {
                if (reducedMotion_ || std::abs(hover - item.hover) < 0.003f) item.hover = hover;
                else { item.hover += (hover - item.hover) * hoverAmount; chunk.moving = true; }
            }
            float selectedWeight = static_cast<int>(item.id) == selected_ ? 1.0f : 0.0f;
            if (item.selection != selectedWeight) chunk.moving |= Approach(item.selection, selectedWeight, selectionStep);
            if (item.highlight != item.highlightTarget)
                chunk.moving |= nebula_highlight::Ease(item.highlight, item.highlightTarget,
                    nebula_highlight::WeightStepAt60Hz, frameDelta_, reducedMotion_);
            item.onScreen = false;
            item.visibility = 0;
            XMVECTOR position = XMLoadFloat3(&item.position);
            XMVECTOR camera = XMVector3Transform(position, view_);
            item.viewX = XMVectorGetX(camera); item.viewY = XMVectorGetY(camera); item.viewZ = XMVectorGetZ(camera);
            if (item.visible && item.viewZ > 1.0f / 32 && item.viewZ < FarPlane &&
                std::abs(item.viewX * px) <= item.viewZ * 1.25f && std::abs(item.viewY * py) <= item.viewZ * 1.25f) {
            item.inverseZ = 1 / item.viewZ;
            float lift = (1 + .05f * item.hover) * (1 + .45f * Smooth(item.selection)) *
                         (1 + .18f * item.highlight * highlightMix_);
            float raw = tileWidth * std::max(item.scale, 0.05f) * lift * kpx * item.inverseZ;
            float radiusSquared = item.viewX * item.viewX + item.viewY * item.viewY + item.viewZ * item.viewZ;
            float radius = radiusSquared < 160000 ? std::sqrt(radiusSquared) : 0;
            float goal = 0;
            if (radiusSquared <= 225) goal = 1;
            else if (radiusSquared < 160000) {
                float focusDistance = XMVectorGetX(XMVector3Length(position - XMLoadFloat3(&target_)));
                float attention = 0.25f + 0.75f * (1 - SmoothRange(12, 60, focusDistance));
                float proximity = 1 - SmoothRange(15, 40, radius);
                float travel = 1 - SmoothRange(60, 400, radius);
                goal = std::max(proximity, attention * travel);
            }
            bool isSelected = static_cast<int>(item.id) == selected_;
            if (!resultsActive_ && !isSelected && selected != itemIndex_.end()) {
                float fromSelection = XMVectorGetX(XMVector3Length(position - selectedPosition));
                goal *= SmoothRange(5, 10, fromSelection);
            }
            if (isSelected || (hoverReady && static_cast<int>(item.id) == hovered_) || item.matched || item.highlight > 0.5f) goal = 1;
            if (item.alpha < 0.12f) goal = 0;
            chunk.moving |= nebula_lod::Attention(item.attention, goal, attentionAmount, reducedMotion_);
            item.edge = nebula_lod::Footprint(raw, item.attention);
            item.contrast = nebula_lod::Contrast(radiusSquared, isSelected || item.highlight > 0.5f);
            item.visibility = nebula_lod::NearVisibility(item.viewZ,
                isSelected, item.highlight * highlightMix_ > .01f, item.matched);
            item.onScreen = item.visibility > .003f;
            if (item.onScreen) {
                if (item.alpha >= 0.12f) {
                    ++chunk.histogram[nebula_lod::Bin(item.edge)];
                    sizeEdges_[index] = item.edge;
                }
            }
            }
        }
    }

    void EmitChunk(size_t which) {
        auto& chunk = chunks_[which];
        auto lower = [](const Want& a, const Want& b) {
            return a.category != b.category ? a.category > b.category : a.priority > b.priority;
        };
        auto offer = [&](Want want) {
            if (chunk.wantCount < chunk.wants.size()) {
                chunk.wants[chunk.wantCount++] = want;
                std::push_heap(chunk.wants.begin(), chunk.wants.begin() + chunk.wantCount, lower);
            } else if (lower(want, chunk.wants[0])) {
                std::pop_heap(chunk.wants.begin(), chunk.wants.end(), lower);
                chunk.wants.back() = want;
                std::push_heap(chunk.wants.begin(), chunk.wants.end(), lower);
            }
        };
        bool hoverReady = reducedMotion_ || prepareTime_ - hoverSince_ >= std::chrono::milliseconds(100);
        for (size_t index = chunk.begin; index < chunk.end; ++index) {
            auto& item = items_[index];
            float drawnEdge = nebula_lod::DrawSize(item.edge, sizeCap0_);
            bool isSelected = static_cast<int>(item.id) == selected_;
            if (item.onScreen) {
                item.visibility = nebula_lod::Visibility(item.viewZ, drawnEdge, static_cast<float>(size_.height),
                    isSelected, item.highlight * highlightMix_ > .01f, item.matched);
                item.onScreen = item.visibility > .003f;
            }
            int oldTier = item.tier;
            chunk.moving |= nebula_lod::UpdateTier(item.tier, item.tierSince, item.edge, caps_,
                static_cast<uint32_t>(frames_) + 1, isSelected, item.alpha < 0.12f || !item.visible, reducedMotion_);
            chunk.lod.flips += oldTier != item.tier;
            bool flat = item.edge <= caps_[0] && !isSelected;
            int best = 0;
            if (flat) {
                item.quality = -1;
                item.repA = item.repB = 0; item.representationMix = 1;
                // Hidden content is already settled when it first becomes eligible.
                item.mosaicMix = item.mosaicReady ? 1.0f : 0.0f; item.thumbnailMix = 1;
            } else {
                item.quality = nebula_lod::Quality(item.tier, item.edge, item.quality, item.hasOriginal);
                int displayQuality = nebula_lod::Quality(3, item.edge, item.quality, item.hasOriginal);
                for (int p = 1; p <= displayQuality; ++p) if (item.cold->imageSlots[p] >= 0) best = p + 2;
                if (best < displayQuality + 2)
                    for (int p = displayQuality + 1; p < 4; ++p) if (item.cold->imageSlots[p] >= 0) { best = p + 2; break; }
                chunk.moving |= nebula_lod::Represent(best, item.repA, item.repB, item.representationMix, frameDelta_, reducedMotion_);
                chunk.moving |= nebula_lod::Residency(item.mosaicMix, item.mosaicReady, frameDelta_, reducedMotion_);
                chunk.moving |= nebula_lod::Residency(item.thumbnailMix, item.cold->imageSlots[0] >= 0, frameDelta_, reducedMotion_);
            }
            if (item.onScreen) {
                ++chunk.lod.visible;
                if (item.edge > caps_[1] && item.cold->imageSlots[0] >= 0)
                    imagePools_[0].slots[item.cold->imageSlots[0]].lastUse = frames_;
                if (item.edge > caps_[2] && item.repA >= 2 && item.cold->imageSlots[item.repA - 2] >= 0)
                    imagePools_[item.repA - 2].slots[item.cold->imageSlots[item.repA - 2]].lastUse = frames_;
                if (item.edge > caps_[2] && item.representationMix < 1 && item.repB >= 2 && item.cold->imageSlots[item.repB - 2] >= 0)
                    imagePools_[item.repB - 2].slots[item.cold->imageSlots[item.repB - 2]].lastUse = frames_;
                int appearance = nebula_lod::AppearanceTier(item.edge, caps_);
                if (appearance == 0) ++chunk.lod.dots;
                if (appearance == 1) ++chunk.lod.mosaics;
                if (appearance == 2) ++chunk.lod.thumbnails;
                if (appearance == 3) ++chunk.lod.previews;
                if (!flat) {
                    int category = isSelected ? 3 : hoverReady && static_cast<int>(item.id) == hovered_ ? 2 :
                                   item.viewZ < 15 ? 1 : 0;
                    for (int p = 0; p <= item.quality; ++p) if (canAdmitFrame_[p] && item.cold->imageSlots[p] < 0 &&
                            !(item.imageLoading & (1u << p)) && !(item.failedTiers & (1u << (p + 2)))) {
                        if (p && (p != item.quality || best >= item.quality + 2)) continue;
                        float area = drawnEdge * drawnEdge * std::min(item.aspect, 1 / item.aspect);
                        float priority = area * std::max(item.contrast, 0.1f) / static_cast<float>(Bc7ChainBytes(MipEdges[p]));
                        offer({priority, index, p + 2, category});
                    }
                }
                EmitItem(item, index, chunk);
            }
        }
    }

    void FrameWorker(size_t index) noexcept {
        SelectFrameCpuSets();
        if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL))
            Error(HRESULT_FROM_WIN32(GetLastError()), L"Set CPU frame-worker priority");
        uint64_t observed = 0;
        for (;;) {
            {
                std::unique_lock lock(frameMutex_);
                frameWake_.wait(lock, [&] { return stopFrameWorkers_ || frameEpoch_ != observed; });
                if (stopFrameWorkers_) return;
                observed = frameEpoch_;
            }
            try { if (emitPass_) EmitChunk(index); else PrepareChunk(index); }
            catch (...) { chunks_[index].failure = std::current_exception(); }
            {
                std::lock_guard lock(frameMutex_);
                if (--workersPending_ == 0) frameDone_.notify_one();
            }
        }
    }

    void RunFrameChunks(bool emit) {
        emitPass_ = emit;
        for (auto& chunk : chunks_) chunk.failure = {};
        {
            std::lock_guard lock(frameMutex_);
            workersPending_ = static_cast<unsigned>(frameWorkers_.size());
            ++frameEpoch_;
            frameWake_.notify_all();
        }
        try { if (emit) EmitChunk(0); else PrepareChunk(0); }
        catch (...) { chunks_[0].failure = std::current_exception(); }
        {
            std::unique_lock lock(frameMutex_);
            frameDone_.wait(lock, [&] { return workersPending_ == 0; });
        }
        for (auto& chunk : chunks_) if (chunk.failure) std::rethrow_exception(chunk.failure);
    }

    void PrepareFrame(Clock::time_point now) {
        prepareTime_ = now;
        originalUsedThisFrame_ = false;
        instanceDirtyFirst_ = SIZE_MAX;
        instanceDirtyLast_ = 0;
        tileCount_ = submittedDots_ = wantCount_ = 0;
        frameLod_ = {sizeof(NbLodStats)};
        for (int p = 0; p < 4; ++p) canAdmitFrame_[p] = AllocateSlot(p + 2, false) >= 0;
        RunFrameChunks(false);
        // Today's histogram updates caps BEFORE today's tier pass.
        nebula_lod::Histogram histogram{};
        for (const auto& chunk : chunks_)
            for (size_t bin = 0; bin < histogram.size(); ++bin) histogram[bin] += chunk.histogram[bin];
        effectsAnimating_ |= nebula_lod::UpdateCaps(caps_, histogram, frameDelta_, reducedMotion_);
        sizeCap0_ = nebula_lod::SizeCap(sizeEdges_);
        RunFrameChunks(true);
        auto lower = [](const Want& a, const Want& b) { return a.category != b.category ? a.category > b.category : a.priority > b.priority; };
        for (auto& chunk : chunks_) {
            if (chunk.failure) std::rethrow_exception(chunk.failure);
            effectsAnimating_ |= chunk.moving;
            originalUsedThisFrame_ |= chunk.originalUsed;
            flips_ += chunk.lod.flips;
            frameLod_.visible += chunk.lod.visible; frameLod_.dots += chunk.lod.dots;
            frameLod_.mosaics += chunk.lod.mosaics; frameLod_.thumbnails += chunk.lod.thumbnails; frameLod_.previews += chunk.lod.previews;
            std::copy(chunk.tiles.begin(), chunk.tiles.end(), instanceOrder_.begin() + tileCount_);
            tileCount_ += chunk.tiles.size();
            submittedDots_ += chunk.submittedDots;
            instanceDirtyFirst_ = std::min(instanceDirtyFirst_, chunk.instanceFirst);
            instanceDirtyLast_ = std::max(instanceDirtyLast_, chunk.instanceLast);
            for (size_t i = 0; i < chunk.wantCount; ++i) {
                const auto& want = chunk.wants[i];
                if (wantCount_ < wants_.size()) {
                    wants_[wantCount_++] = want;
                    std::push_heap(wants_.begin(), wants_.begin() + wantCount_, lower);
                } else if (lower(want, wants_[0])) {
                    std::pop_heap(wants_.begin(), wants_.end(), lower); wants_.back() = want;
                    std::push_heap(wants_.begin(), wants_.end(), lower);
                }
            }
        }
        if (reducedMotion_ || std::chrono::duration<float>(now - animationStart_).count() >= morphDuration_) animating_ = false;
        std::sort(wants_.begin(), wants_.begin() + wantCount_, [](const Want& a, const Want& b) {
            if (a.category != b.category) return a.category > b.category;
            return a.priority != b.priority ? a.priority > b.priority : a.index < b.index;
        });
    }

    bool Project(const Item& item, Hit& hit, float& halfX, float& halfY) {
        if (!item.onScreen) return false;
        float z = item.viewZ;
        float drawn = nebula_lod::DrawSize(item.edge, sizeCap0_);
        float shape = nebula_lod::TileShape(drawn);
        auto half = nebula_lod::HalfExtents(drawn, item.aspect, shape);
        halfX = half[0] * z * worldPerPixel_.x;
        halfY = half[1] * z * worldPerPixel_.y;
        hit.id = static_cast<int>(item.id);
        hit.tier = nebula_lod::AppearanceTier(item.edge, caps_);
        hit.x = item.viewX * projectionPixels_.x * item.inverseZ + size_.width * .5f;
        hit.y = size_.height * .5f - item.viewY * projectionPixels_.y * item.inverseZ;
        hit.halfX = half[0]; hit.halfY = half[1];
        float depth = XMVectorGetZ(projection_.r[2]) + XMVectorGetZ(projection_.r[3]) * item.inverseZ;
        depth += (0.5f + depth * 0.25f - depth) * Presentation(item);
        depth += (0.99999f - depth) * Smooth(item.selection);
        hit.depth = -depth;
        hit.order = 0;
        hit.dot = 1 - shape;
        return hit.x + hit.halfX >= 0 && hit.x - hit.halfX <= size_.width &&
               hit.y + hit.halfY >= 0 && hit.y - hit.halfY <= size_.height;
    }

    int AllocateSlot(int tier, bool retire = true) {
        int poolIndex = tier - 2;
        auto& pool = imagePools_[poolIndex];
        auto* slots = pool.slots.data();
        size_t count = pool.slots.size();
        // Initial streaming must not rescan the occupied prefix for every upload.
        while (pool.unusedSlot < count && slots[pool.unusedSlot].owner >= 0) ++pool.unusedSlot;
        if (pool.unusedSlot < count) return static_cast<int>(pool.unusedSlot);
        if (pool.reuseFrame != frames_) {
            pool.reuseFrame = frames_;
            pool.reusable.clear();
            auto now = Clock::now();
            for (size_t i = 0; i < count; ++i) {
                const auto& slot = slots[i];
                bool orphan = slot.owner < 0 || items_[slot.owner].cold->imageSlots[poolIndex] != static_cast<int>(i);
                if (!orphan) {
                    const auto& owner = items_[slot.owner];
                    bool pinned = owner.onScreen && ((tier == 2 && owner.edge > caps_[1]) ||
                        (owner.edge > caps_[2] && (owner.repA == tier || (owner.repB == tier && owner.representationMix < 1))));
                    if (pinned || now - slot.loadedAt < std::chrono::milliseconds(200)) continue;
                    if (owner.quality >= poolIndex && frames_ - slot.lastUse <= 30) continue;
                }
                pool.reusable.push_back({static_cast<int>(i), slot.lastUse, orphan});
            }
            std::make_heap(pool.reusable.begin(), pool.reusable.end(), ImagePool::Candidate::Later);
        }
        if (pool.reusable.empty()) return -1;
        auto candidate = pool.reusable.front();
        if (retire) {
            std::pop_heap(pool.reusable.begin(), pool.reusable.end(), ImagePool::Candidate::Later);
            pool.reusable.pop_back();
            evictions_ += !candidate.orphan;
        }
        return candidate.index;
    }

    void PhotoDiagnostic(size_t index, int tier, HRESULT hr, const wchar_t* operation, bool failure) {
        auto& item = items_[index];
        if (failure) {
            item.failedTiers |= static_cast<uint8_t>(1u << tier);
            if (tier == 6) item.cold->originalError = hr;
            if (item.failed) return;
            item.failed = true;
            ++failed_;
        } else {
            if (item.cacheWarningLogged) return;
            item.cacheWarningLogged = true;
        }
        const auto& path = tier >= 5 && originals_ ? (*originals_)[index] : (*paths_)[index];
        wchar_t prefix[160]{};
        swprintf_s(prefix, L"Nebula photo %s id=%u tier=%d HRESULT=0x%08X ",
                   failure ? L"decode failure" : L"cache warning", item.id, tier, static_cast<unsigned>(hr));
        std::wstring message = prefix;
        message += operation; message += L" path="; message += path; message += L"\n";
        OutputDebugStringW(message.c_str());
    }

    void UploadResults() {
        if (pendingOriginal_ || (originalLoader_ && originalLoader_->Ready())) return;
        size_t remaining = UploadBudget;
        auto uploadStart = Clock::now();
        for (;;) {
            if (Clock::now() - uploadStart >= std::chrono::milliseconds(1)) break;
            Decoded result;
            {
                std::lock_guard lock(decodeMutex_);
                if (results_.empty()) break;
                const auto& front = results_.front();
                size_t bytes = front.pixels.size() + front.mosaics.size() * MosaicBytesPerSlice;
                if (bytes > remaining) break;
                remaining -= bytes;
                result = std::move(results_.front());
                results_.pop_front();
                readyResults_ = results_.size();
            }
            resultSpace_.notify_one();
            if (result.generation != generation_.load()) continue;
            --pendingJobs_;
            if (result.tier == 1) {
                --mosaicJobs_;
                for (const auto& record : result.mosaics) {
                    auto& item = items_[record.index];
                    if (!item.mosaicComplete) { item.mosaicComplete = true; ++mosaicDone_; }
                    if (FAILED(record.hr)) { PhotoDiagnostic(record.index, 1, record.hr, record.operation, true); continue; }
                    if (FAILED(record.cacheError)) PhotoDiagnostic(record.index, 1, record.cacheError, L"Render cache rejected; source fallback", false);
                    size_t chunk = record.index / MosaicChunkItems;
                    if (!item.mosaicReady) {
                        UINT offset = static_cast<UINT>(record.index % MosaicChunkItems * ResidentMosaicBytes);
                        D3D11_BOX box{offset, 0, 0, offset + static_cast<UINT>(ResidentMosaicBytes), 1, 1};
                        context_->UpdateSubresource(mosaicBuffers_[chunk].Get(), 0, &box, record.pixels.data(), 0, 0);
                        item.mosaicReady = true;
                        ++mosaicReady_;
                    }
                    if (artBuffer_ && !item.artReady) {
                        UINT offset = static_cast<UINT>(record.index * ArtBytesPerSlice);
                        D3D11_BOX box{offset,0,0,offset + static_cast<UINT>(ArtBytesPerSlice),1,1};
                        context_->UpdateSubresource(artBuffer_.Get(), 0, &box, record.pixels.data() + ResidentMosaicBytes, 0, 0);
                        item.artReady = true;
                    }
                    item.aspect = record.aspect;
                    ++uploads_;
                }
                effectsAnimating_ = true;
                continue;
            }
            if (result.index >= items_.size()) continue;
            auto& item = items_[result.index];
            item.imageLoading &= static_cast<uint8_t>(~(1u << (result.tier - 2)));
            if (item.quality < result.tier - 2 || !item.onScreen) continue;
            if (FAILED(result.hr)) {
                PhotoDiagnostic(result.index, result.tier, result.hr, result.operation, true);
                continue;
            }
            if (FAILED(result.cacheError)) PhotoDiagnostic(result.index, result.tier, result.cacheError, L"Render mip cache rejected; source fallback", false);
            bool superseded = false;
            if (result.tier != 2)
                for (int p = result.tier - 1; p < 4; ++p) superseded |= item.cold->imageSlots[p] >= 0;
            if (superseded) continue;
            if (item.cold->imageSlots[result.tier - 2] >= 0) continue;
            int index = AllocateSlot(result.tier);
            if (index < 0) continue;
            auto& pool = imagePools_[result.tier - 2];
            auto& slot = pool.slots[index];
            if (slot.owner >= 0) {
                auto& owner = items_[slot.owner];
                if (owner.cold->imageSlots[result.tier - 2] == index) {
                    owner.cold->imageSlots[result.tier - 2] = -1;
                    if (std::all_of(owner.cold->imageSlots.begin(), owner.cold->imageSlots.end(), [](int value) { return value < 0; })) --frameResident_;
                }
            }
            auto* texture = pool.textures[index / 2048].Get();
            UINT slice = index % 2048;
            for (UINT mip = 0, edge = pool.edge; mip < pool.mipLevels; ++mip, edge = std::max(1u, edge / 2)) {
                context_->UpdateSubresource(texture, D3D11CalcSubresource(mip, slice, pool.mipLevels), nullptr,
                    result.pixels.data() + result.mipOffsets[mip], std::max(1u, (edge + 3) / 4) * 16, 0);
            }
            slot.owner = static_cast<int>(result.index);
            slot.lastUse = frames_;
            slot.uv = result.uv;
            slot.loadedAt = Clock::now();
            ++slot.version;
            if (std::all_of(item.cold->imageSlots.begin(), item.cold->imageSlots.end(), [](int value) { return value < 0; })) ++frameResident_;
            item.cold->imageSlots[result.tier - 2] = index;
            item.aspect = result.aspect;
            ++uploads_;
            effectsAnimating_ = true;
        }
    }

    void QueueRequests() {
        std::lock_guard lock(decodeMutex_);
        size_t queued = requests_.size();
        while (queued--) {
            Request request = std::move(requests_.front());
            requests_.pop_front();
            auto& item = items_[request.index];
            bool superseded = false;
            if (request.tier != 2)
                for (int p = request.tier - 1; p < 4; ++p) superseded |= item.cold->imageSlots[p] >= 0;
            if (item.quality < request.tier - 2 || !item.onScreen || superseded) {
                item.imageLoading &= static_cast<uint8_t>(~(1u << (request.tier - 2)));
                --pendingJobs_;
            } else requests_.push_back(std::move(request));
        }
        for (size_t i = 0; i < wantCount_ && !requests_.full(); ++i) {
            const auto& want = wants_[i];
            auto& item = items_[want.index];
            auto sourcePaths = want.tier == 5 && originals_ && !(*originals_)[want.index].empty() ? originals_ : paths_;
            Request request{generation_.load(), want.index, 1, want.tier, sourcePaths, contentVersions_, cache_};
            if (static_cast<int>(item.id) == selected_) requests_.push_front(std::move(request));
            else requests_.push_back(std::move(request));
            item.imageLoading |= static_cast<uint8_t>(1u << (want.tier - 2));
            ++pendingJobs_;
        }
        while (nextMosaic_ < items_.size() && mosaicJobs_ < MosaicJobs && !mosaicRequests_.full()) {
            size_t count = std::min(MosaicBatch, items_.size() - nextMosaic_);
            mosaicRequests_.push_back({generation_.load(), nextMosaic_, count, 1, paths_, contentVersions_, cache_});
            nextMosaic_ += count;
            ++mosaicJobs_;
            ++pendingJobs_;
        }
        while (artRequested_ && nextMosaic_ == items_.size() && nextArt_ < items_.size() &&
               mosaicJobs_ < MosaicJobs && !mosaicRequests_.full()) {
            size_t count = std::min(MosaicBatch, items_.size() - nextArt_);
            bool needed = false;
            for (size_t i = nextArt_; i < nextArt_ + count; ++i) needed |= !items_[i].artReady && !(items_[i].failedTiers & (1u << 1));
            if (needed) {
                mosaicRequests_.push_back({generation_.load(), nextArt_, count, 1, paths_, contentVersions_, cache_});
                ++mosaicJobs_; ++pendingJobs_;
            }
            nextArt_ += count;
        }
        decodeWake_.notify_all();
    }

    void SortInstances() {
        if (tileCount_ < 2) return;
        auto* source = depthKeys_.data();
        auto* destination = orderScratch_.data();
        uint32_t base = static_cast<uint32_t>(source[instanceOrder_[0]] >> 32), varying = 0;
        bool dense = false;
        for (unsigned shift : {0u, 8u, 16u, 24u}) {
            if (shift && ((varying >> shift) & 255) == 0) continue;
            std::array<uint32_t, 256> offsets{};
            constexpr uint32_t mask = 255;
            // The first gather follows scene indices; subsequent passes stream
            // contiguous packed keys instead of chasing depthKeys[sortedID].
            for (size_t i = 0; i < tileCount_; ++i) {
                uint64_t value = source[dense ? i : instanceOrder_[i]];
                if (shift == 0) varying |= static_cast<uint32_t>(value >> 32) ^ base;
                ++offsets[(value >> (shift + 32)) & mask];
            }
            // Camera depth usually shares its high byte across the whole cloud.
            // A uniform radix byte cannot affect ordering, including stable ties.
            if (((varying >> shift) & mask) == 0) continue;
            uint32_t sum = 0;
            for (auto& offset : offsets) {
                uint32_t count = offset; offset = sum; sum += count;
            }
            for (size_t i = 0; i < tileCount_; ++i) {
                uint64_t value = source[dense ? i : instanceOrder_[i]];
                destination[offsets[(value >> (shift + 32)) & mask]++] = value;
            }
            std::swap(source, destination);
            dense = true;
        }
        if (dense) for (size_t i = 0; i < tileCount_; ++i) instanceOrder_[i] = static_cast<uint32_t>(source[i]);
    }

    void EmitItem(Item& item, size_t index, FrameChunk& chunk) {
            float alpha = VisualAlpha(item);
            float h = item.highlight * highlightMix_ * (1 - Smooth(item.selection));
            Hit hit{};
            float hx, hy;
            if (!Project(item, hit, hx, hy)) return;
            hit.order = static_cast<uint32_t>(index);
            if (item.alpha > 0.35f) {
                chunk.grid.Add(hit, chunk.hits.size());
                chunk.hits.push_back(hit);
            }
            auto reference = [&](int rep) {
                if (rep < 2 || item.cold->imageSlots[rep - 2] < 0) return UINT32_MAX;
                return (static_cast<uint32_t>(rep - 2) << 16) | static_cast<uint32_t>(item.cold->imageSlots[rep - 2]);
            };
            bool original = originalView_ && static_cast<int>(item.id) == selected_ && focusState_.id == selected_ && pixels_ == 0;
            auto appearance = nebula_lod::Appearance(item.edge, caps_);
            Instance instance{item.position, hx, item.packedColor, hy, item.aspect, alpha,
                {Smooth(item.selection), item.hover, h, item.contrast},
                {item.mosaicReady ? static_cast<uint32_t>(index) : UINT32_MAX, reference(item.repA), reference(item.repB),
                 nebula_records::Flags(item.repA, item.repB, pixels_, light_, original, item.artReady,
                     appearance.thumbnail > 0 ? item.cold->imageSlots[0] : -1)},
                {item.representationMix, item.edge, Presentation(item), item.visibility},
                {appearance.level, appearance.thumbnail * Smooth(item.thumbnailMix), appearance.preview, Smooth(item.mosaicMix)}};
            chunk.originalUsed |= original && originalMix_ > .001f && item.alpha > 0.35f;
            size_t output = index;
            if (memcmp(&instanceShadow_[output], &instance, sizeof(instance)) != 0) {
                instanceShadow_[output] = instance;
                chunk.instanceFirst = std::min(chunk.instanceFirst, output);
                chunk.instanceLast = output + 1;
            }
            uint32_t bits = std::bit_cast<uint32_t>(hit.depth);
            uint32_t depth = ~(bits & 0x80000000u ? ~bits : bits ^ 0x80000000u);
            depthKeys_[output] = (static_cast<uint64_t>(depth) << 32) | static_cast<uint32_t>(output);
            chunk.tiles.push_back(static_cast<uint32_t>(output));
            chunk.submittedDots += appearance.level >= 4;
    }

    void RenderFrame() {
        if (sceneResourcesDirty_) {
            EnsureInstanceBuffers(items_.size());
            EnsureMosaics(items_.size());
            sceneResourcesDirty_ = false;
        }
        EnsureArt();
        UpdateCamera();
        for (int p = 0; p < 4; ++p) AllocateSlot(p + 2, false);
        UploadResults();
        PrepareFrame(Clock::now());
        QueueRequests();
        diagnostics_ = {sizeof(NbFrameDiagnostics)};
        diagnostics_.prepareMs = std::chrono::duration<float, std::milli>(Clock::now() - frameStarted_).count();
        auto orderStart = Clock::now();
        SortInstances();
        diagnostics_.orderMs = std::chrono::duration<float, std::milli>(Clock::now() - orderStart).count();
        auto uploadStart = Clock::now();
        Constants constants{};
        XMStoreFloat4x4(&constants.view, view_);
        XMStoreFloat4x4(&constants.projection, projection_);
        constants.viewport = {static_cast<float>(size_.width), static_cast<float>(size_.height), light_ ? 1.0f : 0.0f, 0};
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context_->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), L"Map camera constants");
        memcpy(mapped.pData, &constants, sizeof(constants));
        context_->Unmap(constants_.Get(), 0);
        auto* instanceBuffer = instanceBuffers_[frames_ % instanceBuffers_.size()].Get();
        if (instanceDirtyFirst_ != SIZE_MAX) {
            diagnostics_.instanceUploadBytes = (instanceDirtyLast_ - instanceDirtyFirst_) * sizeof(Instance);
            D3D11_BOX box{static_cast<UINT>(instanceDirtyFirst_ * sizeof(Instance)), 0, 0,
                         static_cast<UINT>(instanceDirtyLast_ * sizeof(Instance)), 1, 1};
            context_->UpdateSubresource(instanceBuffer, 0, &box, instanceShadow_.data() + instanceDirtyFirst_, 0, 0);
        }
        size_t orderFirst = SIZE_MAX, orderLast = 0;
        for (size_t i = 0; i < tileCount_; ++i) if (orderShadow_[i] != instanceOrder_[i]) {
            orderShadow_[i] = instanceOrder_[i]; orderFirst = std::min(orderFirst, i); orderLast = i + 1;
        }
        if (orderFirst != SIZE_MAX) {
            D3D11_BOX box{static_cast<UINT>(orderFirst * 4),0,0,static_cast<UINT>(orderLast * 4),1,1};
            context_->UpdateSubresource(orderBuffer_.Get(), 0, &box, instanceOrder_.data() + orderFirst, 0, 0);
        }

        auto uploadOriginal = [&] {
            if (!pendingOriginal_ && originalLoader_->Ready()) {
                auto image = originalLoader_->Take();
                if (image && image->scene == generation_.load() && image->serial == originalSerial_ && image->id == selected_) {
                    if (FAILED(image->hr)) {
                        focusState_.state = 5; focusState_.pending = 0; focusState_.error = image->hr;
                        PhotoDiagnostic(itemIndex_.at(static_cast<uint32_t>(image->id)), 6, image->hr, image->operation, true);
                    } else {
                        pendingOriginal_ = std::move(image);
                        originalUploadRow_ = 0;
                    }
                }
            }
            if (!pendingOriginal_) return;
            auto& image = *pendingOriginal_;
            if (originalRetiring_) {
                HRESULT ready = context_->GetData(originalRetire_.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
                Check(ready, L"Retire original texture");
                if (ready != S_OK) { effectsAnimating_ = true; return; }
                originalRetiring_ = false;
            }
            if (!originalUploadTexture_) {
                if (focusState_.gpuBytes + image.pixels.size() > nebula_original::Budget) {
                    ID3D11ShaderResourceView* empty = nullptr;
                    context_->PSSetShaderResources(15, 1, &empty);
                    originalView_.Reset(); originalTexture_.Reset();
                    focusState_.gpuBytes = 0;
                    context_->End(originalRetire_.Get());
                    originalRetiring_ = true;
                    effectsAnimating_ = true;
                    return;
                }
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = image.width; desc.Height = image.height;
                desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                Check(device_->CreateTexture2D(&desc, nullptr, &originalUploadTexture_), L"Allocate independent focused-original texture");
            }
            UINT rows = std::min(image.height - originalUploadRow_, static_cast<UINT>((8 * 1024 * 1024) / (image.width * 4)));
            D3D11_BOX region{0, originalUploadRow_, 0, image.width, originalUploadRow_ + rows, 1};
            context_->UpdateSubresource(originalUploadTexture_.Get(), 0, &region,
                image.pixels.data() + static_cast<size_t>(originalUploadRow_) * image.width * 4, image.width * 4, 0);
            originalUploadRow_ += rows;
            effectsAnimating_ = true;
            if (originalUploadRow_ == image.height) {
                originalTexture_ = std::move(originalUploadTexture_);
                originalMix_ = 0;
                originalView_.Reset();
                Check(device_->CreateShaderResourceView(originalTexture_.Get(), nullptr, &originalView_), L"Create focused-original SRV");
                originalUv_ = {static_cast<float>(image.x) / image.sourceWidth, static_cast<float>(image.y) / image.sourceHeight,
                              static_cast<float>(image.x + image.roiWidth) / image.sourceWidth,
                              static_cast<float>(image.y + image.roiHeight) / image.sourceHeight};
                focusState_ = {sizeof(NbFocusStats), image.id, image.sourceWidth, image.sourceHeight, image.width, image.height,
                    image.x, image.y, image.roiWidth, image.roiHeight, image.state, image.decodeMs,
                    static_cast<uint64_t>(image.pixels.size()), S_OK, image.complete ? 0u : 1u};
                items_[itemIndex_.at(static_cast<uint32_t>(image.id))].aspect = static_cast<float>(image.sourceWidth) / image.sourceHeight;
                pendingOriginal_.reset();
            }
        };

        auto updateFocusRegion = [&] {
            if (selected_ < 0 || !originals_ || focusState_.state == 0) { lensDistance_ = distance_; return; }
            auto& item = items_[itemIndex_.at(static_cast<uint32_t>(selected_))];
            float wantedDistance = item.onScreen ? item.viewZ : distance_;
            if (reducedMotion_ || std::abs(wantedDistance - lensDistance_) < 0.002f) lensDistance_ = wantedDistance;
            else { lensDistance_ += (wantedDistance - lensDistance_) * (1 - std::exp(-frameDelta_ / 0.075f)); effectsAnimating_ = true; }
            if (!item.onScreen || focusState_.pending || focusState_.state == 0 || focusState_.state == 3 || !focusState_.sourceWidth) return;
            Hit hit{}; float hx, hy;
            if (!Project(item, hit, hx, hy) || std::max(hit.halfX, hit.halfY) * 2 <= 2048) return;
            std::array<float, 4> uv{
                std::clamp((0 - hit.x + hit.halfX) / (2 * hit.halfX), 0.0f, 1.0f),
                std::clamp((0 - hit.y + hit.halfY) / (2 * hit.halfY), 0.0f, 1.0f),
                std::clamp((size_.width - hit.x + hit.halfX) / (2 * hit.halfX), 0.0f, 1.0f),
                std::clamp((size_.height - hit.y + hit.halfY) / (2 * hit.halfY), 0.0f, 1.0f)};
            for (int axis = 0; axis < 2; ++axis) {
                float extent = static_cast<float>(axis ? focusState_.sourceHeight : focusState_.sourceWidth);
                uv[axis] = std::floor(uv[axis] * extent / 512) * 512 / extent;
                uv[axis + 2] = std::min(1.0f, std::ceil(uv[axis + 2] * extent / 512) * 512 / extent);
            }
            if (uv == requestedUv_) return;
            requestedUv_ = uv;
            focusState_.pending = 1;
            originalSerial_ = originalLoader_->RequestImage(generation_, selected_, (*originals_)[itemIndex_.at(static_cast<uint32_t>(selected_))], uv);
        };
        uploadOriginal();
        effectsAnimating_ |= nebula_lod::Residency(originalMix_, originalView_ != nullptr, frameDelta_, reducedMotion_);
        updateFocusRegion();
        diagnostics_.uploadMs = std::chrono::duration<float, std::milli>(Clock::now() - uploadStart).count();
        auto submitStart = Clock::now();
        BeginGpuTiming();
        bool glass = VisibleGlass();
        if (glass) EnsureGlassTargets();
        bool dof = focusDof_ && selected_ >= 0 && originalView_ && originalUsedThisFrame_;
        if ((!focusDof_ || selected_ < 0) && linearDepth_.texture) {
            linearDepth_ = {}; protectionTarget_ = {}; dofHalfA_ = {}; dofHalfB_ = {}; dofOutput_ = {};
            if (!glass) sceneTarget_ = {};
        }
        if (dof) EnsureDepthTargets();
        ID3D11RenderTargetView* canvasTarget = glass || dof ? sceneTarget_.target.Get() : targetView_.Get();
        const float dark[]{Srgb(20.0f / 255), Srgb(20.0f / 255), Srgb(22.0f / 255), 1};
        const float light[]{Srgb(239.0f / 255), Srgb(239.0f / 255), Srgb(237.0f / 255), 1};
        context_->ClearRenderTargetView(canvasTarget, light_ ? light : dark);
        context_->ClearDepthStencilView(depthView_.Get(), D3D11_CLEAR_DEPTH, 0, 0);
        context_->OMSetRenderTargets(1, &canvasTarget, depthView_.Get());
        context_->OMSetDepthStencilState(depthState_.Get(), 0);
        context_->OMSetBlendState(tileBlend_.Get(), nullptr, 0xffffffff);
        context_->RSSetState(rasterizer_.Get());
        D3D11_VIEWPORT viewport{0, 0, static_cast<float>(size_.width), static_cast<float>(size_.height), 0, 1};
        context_->RSSetViewports(1, &viewport);
        if (dof) {
            const float zero[]{0,0,0,0};
            context_->ClearRenderTargetView(linearDepth_.target.Get(), zero);
            context_->ClearRenderTargetView(protectionTarget_.target.Get(), zero);
            ID3D11RenderTargetView* targets[]{canvasTarget, linearDepth_.target.Get(), protectionTarget_.target.Get()};
            context_->OMSetRenderTargets(3, targets, depthView_.Get());
        }
        XMFLOAT4 sceneLens[]{ {dof ? lensDistance_ : distance_, originalView_ ? Smooth(originalMix_) : 0.0f, dof ? 1.0f : 0.0f, atmosphere_ ? 1.0f : 0.0f},
                             originalUv_ };
        Check(context_->Map(sceneLens_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), L"Map scene lens");
        memcpy(mapped.pData, sceneLens, sizeof(sceneLens)); context_->Unmap(sceneLens_.Get(), 0);
        context_->PSSetConstantBuffers(1, 1, sceneLens_.GetAddressOf());
        context_->PSSetShaderResources(15, 1, originalView_.GetAddressOf());
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11ShaderResourceView* geometry[]{instanceView_.Get(), orderView_.Get()};
        context_->VSSetShaderResources(20, 2, geometry);
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        context_->VSSetConstantBuffers(0, 1, constants_.GetAddressOf());
        context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* resources[]{
            imagePools_[0].views[0].Get(), imagePools_[0].views[1].Get(), imagePools_[0].views[2].Get(), imagePools_[0].views[3].Get(),
            imagePools_[1].views[0].Get(), imagePools_[2].views[0].Get(), imagePools_[3].views[0].Get(),
            mosaicViews_[0].Get(), mosaicViews_[1].Get(), mosaicViews_[2].Get(), mosaicViews_[3].Get()
        };
        context_->PSSetShaderResources(0, 11, resources);
        context_->PSSetShaderResources(11, 1, artView_.GetAddressOf());
        context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
        context_->PSSetSamplers(1, 1, pointSampler_.GetAddressOf());
        if (tileCount_ != 0) {
            context_->OMSetRenderTargets(0, nullptr, depthView_.Get());
            context_->PSSetShader(opaqueDepthShader_.Get(), nullptr, 0);
            context_->DrawInstanced(6, static_cast<UINT>(tileCount_), 0, 0);
            ID3D11RenderTargetView* targets[]{canvasTarget, dof ? linearDepth_.target.Get() : nullptr,
                                            dof ? protectionTarget_.target.Get() : nullptr};
            context_->OMSetRenderTargets(dof ? 3 : 1, targets, depthView_.Get());
            context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
            context_->DrawInstanced(6, static_cast<UINT>(tileCount_), 0, 0);
        }
        if (dof) CompositeDepth();
        canvasTarget = glass || dof ? sceneTarget_.target.Get() : targetView_.Get();
        context_->OMSetRenderTargets(1, &canvasTarget, depthView_.Get());
        context_->RSSetViewports(1, &viewport);
        context_->OMSetBlendState(tileBlend_.Get(), nullptr, 0xffffffff);
        DrawLines();
        if (glass) CompositeGlass();
        else if (dof) {
            context_->IASetInputLayout(nullptr); context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context_->VSSetShader(fullscreenShader_.Get(), nullptr, 0);
            context_->OMSetDepthStencilState(noDepth_.Get(), 0);
            context_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
            GlassConstants copy{};
            GlassPass(targetView_.Get(), size_.width, size_.height, compositeShader_.Get(), sceneTarget_.view.Get(), nullptr, copy);
        }
        if (currentTimer_) {
            context_->End(currentTimer_->end.Get());
            context_->End(currentTimer_->disjoint.Get());
            currentTimer_->pending = true;
        }
        frameLod_.cpuMs = std::chrono::duration<float, std::milli>(Clock::now() - frameStarted_).count();
        diagnostics_.submitMs = std::chrono::duration<float, std::milli>(Clock::now() - submitStart).count();
        diagnostics_.tiles = tileCount_ - submittedDots_; diagnostics_.dots = submittedDots_;
        diagnostics_.reserved = (cameraAnimating_ ? 1u : 0u) | (animating_ ? 2u : 0u) | (effectsAnimating_ ? 4u : 0u);
        Check(swapChain_->Present(1, 0), L"Present");
        Check(device_->GetDeviceRemovedReason(), L"D3D11 device health");
        ++frames_;
        auto now = Clock::now();
        {
            std::lock_guard lock(statusMutex_);
            float seconds = std::chrono::duration<float>(now - lastPresent_).count();
            if (seconds > 0 && seconds < 0.5f) fps_ = fps_ ? fps_ * 0.8f + 0.2f / seconds : 1 / seconds;
            else fps_ = 0;
            lastPresent_ = now;
        }
        frameLod_.frameMs = std::chrono::duration<float, std::milli>(Clock::now() - frameStarted_).count();
        PublishStats(true);
    }

    void BeginGpuTiming() {
        for (auto& timer : gpuTimers_) if (timer.pending) {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
            HRESULT ready = context_->GetData(timer.disjoint.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            Check(ready, L"Read GPU disjoint query");
            if (ready != S_OK) continue;
            uint64_t start = 0, end = 0;
            HRESULT a = context_->GetData(timer.start.Get(), &start, sizeof(start), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            HRESULT b = context_->GetData(timer.end.Get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            Check(a, L"Read GPU start timestamp"); Check(b, L"Read GPU end timestamp");
            if (a != S_OK || b != S_OK) continue;
            if (!disjoint.Disjoint && disjoint.Frequency && timer.frame > gpuFrame_) {
                gpuMs_ = static_cast<float>((end - start) * 1000.0 / disjoint.Frequency);
                gpuFrame_ = timer.frame;
            }
            timer.pending = false;
        }
        auto& timer = gpuTimers_[frames_ % gpuTimers_.size()];
        currentTimer_ = timer.pending ? nullptr : &timer;
        if (currentTimer_) {
            timer.frame = frames_ + 1;
            context_->Begin(timer.disjoint.Get());
            context_->End(timer.start.Get());
        }
    }

    void DrawLines() {
        if (lines_.empty() || highlightMix_ <= nebula_highlight::LineVisibleMix) return;
        lineVertices_.clear();
        for (const auto& line : lines_) {
            const auto& a = items_[itemIndex_.at(line.a)];
            const auto& b = items_[itemIndex_.at(line.b)];
            if (!a.visible || !b.visible || line.weight == 0) continue;
            float alpha = nebula_highlight::LineAlpha * line.weight * highlightMix_;
            lineVertices_.push_back({a.position, alpha});
            lineVertices_.push_back({b.position, alpha});
        }
        if (lineVertices_.empty()) return;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context_->Map(lineBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), L"Map constellation line vertices");
        memcpy(mapped.pData, lineVertices_.data(), lineVertices_.size() * sizeof(LineVertex));
        context_->Unmap(lineBuffer_.Get(), 0);
        context_->IASetInputLayout(lineLayout_.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        UINT stride = sizeof(LineVertex), offset = 0;
        context_->IASetVertexBuffers(0, 1, lineBuffer_.GetAddressOf(), &stride, &offset);
        context_->VSSetShader(lineVertexShader_.Get(), nullptr, 0);
        context_->VSSetConstantBuffers(0, 1, constants_.GetAddressOf());
        context_->PSSetShader(linePixelShader_.Get(), nullptr, 0);
        context_->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());
        context_->OMSetDepthStencilState(lineDepth_.Get(), 0);
        context_->Draw(static_cast<UINT>(lineVertices_.size()), 0);
        context_->OMSetDepthStencilState(depthState_.Get(), 0);
    }

    void PublishStats(bool presented) {
        NbStats snapshot{sizeof(NbStats)};
        snapshot.items = static_cast<uint32_t>(items_.size());
        snapshot.frames = frames_;
        snapshot.fps = fps_;
        snapshot.resident = frameResident_;
        snapshot.pending = pendingJobs_ + focusState_.pending;
        snapshot.failed = failed_;
        frameLod_.items = snapshot.items;
        frameLod_.mosaicReady = mosaicReady_;
        frameLod_.mosaicPending = static_cast<uint32_t>(items_.size()) - mosaicDone_;
        frameLod_.pending = pendingJobs_;
        frameLod_.failed = failed_;
        frameLod_.thumbnailResident = 0; frameLod_.previewResident = 0;
        for (const auto& slot : imagePools_[0].slots) frameLod_.thumbnailResident += slot.owner >= 0;
        for (size_t pool = 1; pool < imagePools_.size(); ++pool)
            for (const auto& slot : imagePools_[pool].slots) frameLod_.previewResident += slot.owner >= 0;
        frameLod_.cap0 = caps_[0]; frameLod_.cap1 = caps_[1]; frameLod_.cap2 = caps_[2];
        frameLod_.frames = frames_; frameLod_.gpuFrame = gpuFrame_;
        frameLod_.gpuMs = gpuMs_;
        frameLod_.flips = flips_; frameLod_.uploads = uploads_; frameLod_.evictions = evictions_;
        frameLod_.gpuBytes = ImagePoolBytes + items_.size() * ResidentMosaicBytes + artCapacity_ * ArtBytesPerSlice +
            instanceCapacity_ * (sizeof(Instance) + sizeof(uint32_t)) +
            static_cast<uint64_t>(std::max(size_.width, 1)) * std::max(size_.height, 1) * 12;
        if (sceneTarget_.texture) frameLod_.gpuBytes += static_cast<uint64_t>(size_.width) * size_.height * 4 +
            static_cast<uint64_t>(blurWidth_) * blurHeight_ * 8;
        frameLod_.gpuBytes += focusState_.gpuBytes;
        if (linearDepth_.texture) frameLod_.gpuBytes += static_cast<uint64_t>(size_.width) * size_.height * 11;
        std::lock_guard lock(statusMutex_);
        stats_ = snapshot;
        frameLod_.pickMs = lodSnapshot_.pickMs;
        lodSnapshot_ = frameLod_;
        focusSnapshot_ = focusState_;
        diagnosticsSnapshot_ = diagnostics_;
        if (presented) {
            for (size_t i = 0; i < chunks_.size(); ++i) {
                publishedPicks_[i].hits.swap(chunks_[i].hits);
                std::swap(publishedPicks_[i].grid, chunks_[i].grid);
            }
            XMFLOAT4X4 matrix;
            XMStoreFloat4x4(&matrix, XMMatrixTranspose(view_ * projection_));
            memcpy(viewSnapshot_.viewProj, &matrix, sizeof(matrix));
            viewSnapshot_.width = static_cast<float>(size_.width);
            viewSnapshot_.height = static_cast<float>(size_.height);
            viewSnapshot_.scaleX = size_.scaleX;
            viewSnapshot_.scaleY = size_.scaleY;
            viewSnapshot_.pixelsPerUnitAtDistance1 = size_.height * 0.5f * XMVectorGetY(projection_.r[1]);
            viewReady_ = true;
        }
    }
};

template<class F> int Api(void* handle, F&& operation) noexcept {
    if (!handle) return E_POINTER;
    auto* renderer = static_cast<Renderer*>(handle);
    try {
        HRESULT hr = operation(*renderer);
        if (FAILED(hr)) renderer->Error(hr, L"Native renderer operation");
        return hr;
    } catch (const std::bad_alloc&) {
        renderer->Error(E_OUTOFMEMORY, L"Native command allocation");
        return E_OUTOFMEMORY;
    } catch (...) {
        renderer->Error(E_FAIL, L"Unexpected native command failure");
        return E_FAIL;
    }
}
} // namespace

int nb_create(void* panelInspectable, void** renderer) {
    if (!renderer) return E_POINTER;
    *renderer = nullptr;
    if (!panelInspectable) return E_POINTER;
    try {
        auto result = std::make_unique<Renderer>();
        // This is the WinUI 3 interface from Microsoft's restored NuGet header, not UWP.
        static_assert(__uuidof(ISwapChainPanelNative).Data1 == 0x63aad0b8);
        Check(static_cast<IUnknown*>(panelInspectable)->QueryInterface(IID_PPV_ARGS(&result->panel)),
              L"Query WinUI 3 ISwapChainPanelNative");
        Check(result->Start(), L"Initialize native renderer");
        Check(result->Attach(), L"Attach native renderer");
        *renderer = result.release();
        return S_OK;
    } catch (const HrFailure& failure) { return failure.hr; }
      catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}

void nb_destroy(void* renderer) {
    if (!renderer) return;
    auto* instance = static_cast<Renderer*>(renderer);
    if (instance->panel) {
        HRESULT hr = instance->panel->SetSwapChain(nullptr);
        if (FAILED(hr)) instance->Error(hr, L"Detach SwapChainPanel");
        instance->panel.Reset();
    }
    delete instance;
}

int nb_resize(void* renderer, int width, int height, float scaleX, float scaleY) {
    return Api(renderer, [&](Renderer& r) -> HRESULT {
        if (width < 0 || height < 0 || width > 16384 || height > 16384 ||
            static_cast<uint64_t>(width) * height > 16777216 ||
            !Finite(scaleX) || !Finite(scaleY) || scaleX < 0.1f || scaleY < 0.1f ||
            scaleX > 16 || scaleY > 16) return E_INVALIDARG;
        return r.Command([&](Commands& c) { c.size = Size{width, height, scaleX, scaleY}; });
    });
}
int nb_set_scene(void* renderer, const NbItem* items, int count) {
    return Api(renderer, [&](Renderer& r) { return r.Scene(items, count); });
}
int nb_set_positions(void* renderer, const float* xyz, int count) {
    return Api(renderer, [&](Renderer& r) { return r.Positions(xyz, count); });
}
int nb_set_filter(void* renderer, const unsigned char* mask, int count) {
    return Api(renderer, [&](Renderer& r) { return r.Filter(mask, count); });
}
int nb_select(void* renderer, int id) {
    return Api(renderer, [&](Renderer& r) { return r.Select(id); });
}
int nb_pick(void* renderer, float x, float y, int* id) {
    return Api(renderer, [&](Renderer& r) { return r.Pick(x, y, id); });
}
int nb_orbit(void* renderer, float dx, float dy) {
    return Api(renderer, [&](Renderer& r) -> HRESULT {
        if (!Finite(dx) || !Finite(dy)) return E_INVALIDARG;
        return r.Command([&](Commands& c) {
            c.orbitX = std::clamp(c.orbitX + dx, -10000.0f, 10000.0f);
            c.orbitY = std::clamp(c.orbitY + dy, -10000.0f, 10000.0f);
        });
    });
}
int nb_pan(void* renderer, float dx, float dy) {
    return Api(renderer, [&](Renderer& r) -> HRESULT {
        if (!Finite(dx) || !Finite(dy)) return E_INVALIDARG;
        return r.Command([&](Commands& c) {
            c.panX = std::clamp(c.panX + dx, -10000.0f, 10000.0f);
            c.panY = std::clamp(c.panY + dy, -10000.0f, 10000.0f);
        });
    });
}
int nb_zoom(void* renderer, float delta) {
    return Api(renderer, [&](Renderer& r) -> HRESULT {
        if (!Finite(delta)) return E_INVALIDARG;
        return r.Command([&](Commands& c) { c.zoom = std::clamp(c.zoom + delta, -12000.0f, 12000.0f); });
    });
}
int nb_fit(void* renderer) {
    return Api(renderer, [](Renderer& r) { return r.Command([](Commands& c) {
        c.fit = true; c.back = false; c.focus.reset(); c.focusSet.reset();
    }); });
}
int nb_set_theme(void* renderer, int light) {
    return Api(renderer, [&](Renderer& r) -> HRESULT {
        if (light != 0 && light != 1) return E_INVALIDARG;
        return r.Command([&](Commands& c) { c.theme = light; });
    });
}
int nb_get_stats(void* renderer, NbStats* stats) {
    // Preserve the actionable render/decode error when the shell polls a faulted renderer.
    if (!renderer) return E_POINTER;
    return static_cast<Renderer*>(renderer)->Stats(stats);
}
int nb_get_error(void* renderer, wchar_t* buffer, int capacity) {
    if (!renderer) return E_POINTER;
    return static_cast<Renderer*>(renderer)->GetError(buffer, capacity);
}
int nb_set_glass(void* renderer, const NbGlassRect* rects, int count) {
    return Api(renderer, [&](Renderer& r) { return r.Glass(rects, count); });
}
int nb_set_glass_panels(void* renderer, const NbGlassPanel* panels, int count) {
    return Api(renderer, [&](Renderer& r) { return r.GlassPanels(panels, count); });
}
int nb_hover(void* renderer, int id) {
    return Api(renderer, [&](Renderer& r) { return r.Hover(id); });
}
int nb_focus(void* renderer, int id) {
    return Api(renderer, [&](Renderer& r) { return r.Focus(id); });
}
int nb_back(void* renderer) {
    return Api(renderer, [](Renderer& r) { return r.Command([](Commands& c) {
        c.back = true; c.fit = false; c.focus.reset(); c.focusSet.reset();
    }); });
}
int nb_set_reduced_motion(void* renderer, int enabled) {
    return Api(renderer, [&](Renderer& r) -> HRESULT {
        if (enabled != 0 && enabled != 1) return E_INVALIDARG;
        return r.Command([&](Commands& c) { c.reducedMotion = enabled; });
    });
}
int nb_set_pixels(void* renderer, int level) {
    return Api(renderer, [&](Renderer& r) -> HRESULT {
        if (level < 0 || level > 5) return E_INVALIDARG;
        return r.Command([&](Commands& c) { c.pixels = level; });
    });
}
int nb_set_results(void* renderer, const NbResult* results, int count) {
    return Api(renderer, [&](Renderer& r) { return r.Results(results, count); });
}
int nb_clear_results(void* renderer) {
    return Api(renderer, [](Renderer& r) { return r.ClearResults(); });
}
int nb_set_highlight(void* renderer, const uint32_t* ids, const float* weights, int count) {
    return Api(renderer, [&](Renderer& r) { return r.Highlight(ids, weights, count); });
}
int nb_set_lines(void* renderer, const NbLine* lines, int count) {
    return Api(renderer, [&](Renderer& r) { return r.Lines(lines, count); });
}
int nb_focus_set(void* renderer, const uint32_t* ids, int count) {
    return Api(renderer, [&](Renderer& r) { return r.FocusSet(ids, count); });
}
int nb_get_view(void* renderer, NbView* view) {
    return Api(renderer, [&](Renderer& r) { return r.View(view); });
}
int nb_pick_lod(void* renderer, float x, float y, int* id, int* tier) {
    return Api(renderer, [&](Renderer& r) -> HRESULT {
        if (!tier) return E_INVALIDARG;
        return r.Pick(x, y, id, tier);
    });
}
int nb_get_lod_stats(void* renderer, NbLodStats* stats) {
    return Api(renderer, [&](Renderer& r) { return r.LodStats(stats); });
}
int nb_set_scene_v2(void* renderer, const NbItemV2* items, int count) {
    return Api(renderer, [&](Renderer& r) { return r.Scene(nullptr, count, items); });
}
int nb_get_focus_stats(void* renderer, NbFocusStats* stats) {
    return Api(renderer, [&](Renderer& r) { return r.FocusStats(stats); });
}
int nb_set_depth_options(void* renderer, int atmosphere, int focusDof) {
    return Api(renderer, [&](Renderer& r) -> HRESULT {
        if ((atmosphere != 0 && atmosphere != 1) || (focusDof != 0 && focusDof != 1)) return E_INVALIDARG;
        return r.Command([&](Commands& c) { c.depthOptions = {atmosphere, focusDof}; });
    });
}
int nb_get_frame_diagnostics(void* renderer, NbFrameDiagnostics* diagnostics) {
    return Api(renderer, [&](Renderer& r) { return r.Diagnostics(diagnostics); });
}
