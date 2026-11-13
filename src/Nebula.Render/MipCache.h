#pragma once
#include <windows.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <array>
#include <vector>
#include <string>
#include <mutex>
#include <unordered_map>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cstring>
#include "vendor\bc7enc\bc7enc.h"

namespace nebula_cache {
inline constexpr uint32_t Recipe = 6;
inline constexpr uint32_t CodecBc7 = 99; // DXGI_FORMAT_BC7_UNORM_SRGB
inline constexpr uint64_t DiskBudget = 8ull * 1024 * 1024 * 1024;
struct Failure { HRESULT hr; };
inline void Check(bool value) {
    if (!value) { DWORD error = GetLastError(); throw Failure{error ? HRESULT_FROM_WIN32(error) : E_FAIL}; }
}
inline uint32_t Crc(const void* data, size_t count) {
    static const auto table = [] {
        std::array<uint32_t, 256> values{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1)));
            values[i] = value;
        }
        return values;
    }();
    auto* bytes = static_cast<const unsigned char*>(data);
    uint32_t crc = ~0u;
    for (size_t i = 0; i < count; ++i) {
        crc = (crc >> 8) ^ table[(crc ^ bytes[i]) & 255];
    }
    return ~crc;
}
inline uint64_t Hash(const void* data, size_t count, uint64_t value = 1469598103934665603ull) {
    auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < count; ++i) value = (value ^ bytes[i]) * 1099511628211ull;
    return value;
}
using ContentKey = std::array<unsigned char, 32>;
inline ContentKey SourceKey(const std::wstring& path, uint64_t contentVersion) {
    struct File { HANDLE value; ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
    struct Algorithm { BCRYPT_ALG_HANDLE value = nullptr; ~Algorithm() { if (value) BCryptCloseAlgorithmProvider(value, 0); } };
    struct HashObject { BCRYPT_HASH_HANDLE value = nullptr; ~HashObject() { if (value) BCryptDestroyHash(value); } };
    File file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    Check(file.value != INVALID_HANDLE_VALUE);
    Algorithm algorithm; HashObject hash;
    if (BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) throw Failure{E_FAIL};
    // CNG owns the backing allocation until BCryptDestroyHash. A separately
    // scoped vector could otherwise free that storage before the hash destructor.
    if (BCryptCreateHash(algorithm.value, &hash.value, nullptr, 0, nullptr, 0, 0) < 0) throw Failure{E_FAIL};
    std::array<unsigned char, 32768> bytes{};
    for (;;) {
        DWORD read = 0;
        Check(ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) != FALSE);
        if (!read) break;
        if (BCryptHashData(hash.value, bytes.data(), read, 0) < 0) throw Failure{E_FAIL};
    }
    uint32_t recipe = Recipe;
    if (BCryptHashData(hash.value, reinterpret_cast<PUCHAR>(&contentVersion), sizeof(contentVersion), 0) < 0 ||
        BCryptHashData(hash.value, reinterpret_cast<PUCHAR>(&recipe), sizeof(recipe), 0) < 0) throw Failure{E_FAIL};
    ContentKey key{};
    if (BCryptFinishHash(hash.value, key.data(), static_cast<ULONG>(key.size()), 0) < 0) throw Failure{E_FAIL};
    return key;
}
struct Payload {
    uint32_t edge = 0, codec = 0, levels = 0;
    float aspect = 1;
    uint32_t sourceWidth = 0, sourceHeight = 0, orientation = 1;
    std::array<uint32_t, 11> offsets{};
    std::vector<unsigned char> bytes;
};
inline Payload Encode(const std::vector<unsigned char>& bgra, uint32_t edge, float aspect) {
    static std::once_flag initialized;
    std::call_once(initialized, bc7enc_compress_block_init);
    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    params.m_mode_mask = (1u << 1) | (1u << 6);
    params.m_max_partitions = 16;
    bc7enc_compress_block_params_init_linear_weights(&params);
    Payload result;
    result.edge = edge; result.codec = CodecBc7; result.aspect = aspect;
    std::vector<unsigned char> pixels = bgra;
    uint32_t size = edge;
    for (;;) {
        result.offsets[result.levels++] = static_cast<uint32_t>(result.bytes.size());
        uint32_t blocks = std::max(1u, (size + 3) / 4);
        size_t start = result.bytes.size();
        result.bytes.resize(start + static_cast<size_t>(blocks) * blocks * 16);
        for (uint32_t y = 0; y < blocks; ++y) for (uint32_t x = 0; x < blocks; ++x) {
            std::array<color_rgba, 16> block{};
            for (uint32_t by = 0; by < 4; ++by) for (uint32_t bx = 0; bx < 4; ++bx) {
                auto* source = pixels.data() + (std::min(y * 4 + by, size - 1) * size + std::min(x * 4 + bx, size - 1)) * 4;
                auto& destination = block[by * 4 + bx].m_c;
                destination[0] = source[2]; destination[1] = source[1]; destination[2] = source[0]; destination[3] = source[3];
            }
            bc7enc_compress_block(result.bytes.data() + start + (y * blocks + x) * 16, block.data(), &params);
        }
        if (size == 1) break;
        uint32_t next = size / 2;
        std::vector<unsigned char> smaller(static_cast<size_t>(next) * next * 4);
        for (uint32_t y = 0; y < next; ++y) for (uint32_t x = 0; x < next; ++x) {
            float sum[3]{}, alpha = 0;
            for (uint32_t dy = 0; dy < 2; ++dy) for (uint32_t dx = 0; dx < 2; ++dx) {
                auto* source = pixels.data() + ((y * 2 + dy) * size + x * 2 + dx) * 4;
                float a = source[3] / 255.0f;
                for (int c = 0; c < 3; ++c) {
                    float v = source[c] / 255.0f;
                    sum[c] += (v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f)) * a;
                }
                alpha += a;
            }
            auto* destination = smaller.data() + (y * next + x) * 4;
            for (int c = 0; c < 3; ++c) {
                float v = alpha > 0 ? sum[c] / alpha : 0;
                v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f;
                destination[c] = static_cast<unsigned char>(std::clamp(std::lround(v * 255), 0l, 255l));
            }
            destination[3] = static_cast<unsigned char>(std::lround(alpha * 255 / 4));
        }
        pixels = std::move(smaller); size = next;
    }
    result.offsets[result.levels] = static_cast<uint32_t>(result.bytes.size());
    return result;
}

#pragma pack(push, 1)
struct Header {
    uint64_t magic = 0x314b434150424e; // NBP ACK1
    uint32_t version = 1, size = sizeof(Header), recipe = Recipe, colorSpace = 1;
    uint64_t library = 0;
    uint32_t crc = 0, reserved = 0;
};
struct Entry {
    ContentKey key{};
    uint64_t offset = 0;
    uint32_t edge = 0, codec = 0, levels = 0, length = 0;
    float aspect = 1;
    uint32_t sourceWidth = 0, sourceHeight = 0, orientation = 1;
    std::array<uint32_t, 11> mips{};
    uint32_t dataCrc = 0, crc = 0;
};
#pragma pack(pop)
struct Key {
    ContentKey content;
    uint32_t edge;
    bool operator==(const Key&) const = default;
};
struct KeyHash {
    size_t operator()(const Key& key) const {
        uint64_t value = 0; memcpy(&value, key.content.data(), sizeof(value));
        return static_cast<size_t>(value ^ (uint64_t(key.edge) << 32));
    }
};
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
};
struct Mapping {
    void* address = nullptr;
    ~Mapping() { if (address) UnmapViewOfFile(address); }
};

class Store {
    std::filesystem::path root_;
    uint64_t library_;
    std::mutex mutex_;
    Handle pack_, journal_;
    std::unordered_map<Key, Entry, KeyHash> entries_;
    bool opened_ = false;
    HRESULT warning_ = S_OK;
    HRESULT openFailure_ = S_OK;
    HRESULT Failed(HRESULT hr) {
        if (!opened_) {
            if (pack_.value != INVALID_HANDLE_VALUE) CloseHandle(pack_.value);
            if (journal_.value != INVALID_HANDLE_VALUE) CloseHandle(journal_.value);
            pack_.value = journal_.value = INVALID_HANDLE_VALUE;
            openFailure_ = hr;
        }
        return hr;
    }
    void Open() {
        if (FAILED(openFailure_)) throw Failure{openFailure_};
        if (opened_) return;
        std::filesystem::create_directories(root_);
        wchar_t name[32]{};
        swprintf_s(name, L"%016llx", static_cast<unsigned long long>(library_));
        auto base = root_ / name;
        auto packPath = base; packPath += L".pack";
        auto journalPath = base; journalPath += L".journal";
        pack_.value = CreateFileW(packPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(pack_.value != INVALID_HANDLE_VALUE);
        journal_.value = CreateFileW(journalPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(journal_.value != INVALID_HANDLE_VALUE);
        LARGE_INTEGER length{};
        Check(GetFileSizeEx(pack_.value, &length) != FALSE);
        Header header;
        DWORD transferred = 0;
        if (length.QuadPart == 0) {
            header.library = library_;
            header.crc = Crc(&header, offsetof(Header, crc));
            Check(WriteFile(pack_.value, &header, sizeof(header), &transferred, nullptr) && transferred == sizeof(header));
            Check(FlushFileBuffers(pack_.value) != FALSE);
            auto temporary = base; temporary += L".manifest.tmp";
            auto manifest = base; manifest += L".manifest";
            Handle file;
            file.value = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            Check(file.value != INVALID_HANDLE_VALUE);
            Check(WriteFile(file.value, &header, sizeof(header), &transferred, nullptr) && transferred == sizeof(header));
            Check(FlushFileBuffers(file.value) != FALSE);
            CloseHandle(file.value); file.value = INVALID_HANDLE_VALUE;
            Check(MoveFileExW(temporary.c_str(), manifest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE);
        } else {
            Check(ReadFile(pack_.value, &header, sizeof(header), &transferred, nullptr) != FALSE);
            if (transferred != sizeof(header) || header.magic != Header{}.magic || header.version != 1 ||
                header.size != sizeof(Header) || header.recipe != Recipe || header.library != library_ ||
                header.crc != Crc(&header, offsetof(Header, crc))) throw Failure{HRESULT_FROM_WIN32(ERROR_INVALID_DATA)};
            auto manifest = base; manifest += L".manifest";
            Handle pointer;
            pointer.value = CreateFileW(manifest.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            Check(pointer.value != INVALID_HANDLE_VALUE);
            Header published;
            Check(ReadFile(pointer.value, &published, sizeof(published), &transferred, nullptr) != FALSE);
            if (transferred != sizeof(published) || memcmp(&published, &header, sizeof(header)) != 0)
                throw Failure{HRESULT_FROM_WIN32(ERROR_INVALID_DATA)};
        }
        Entry entry;
        for (;;) {
            Check(ReadFile(journal_.value, &entry, sizeof(entry), &transferred, nullptr) != FALSE);
            if (!transferred) break;
            if (transferred != sizeof(entry)) {
                warning_ = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                LARGE_INTEGER rewind{}; rewind.QuadPart = -static_cast<LONGLONG>(transferred);
                Check(SetFilePointerEx(journal_.value, rewind, nullptr, FILE_CURRENT) != FALSE);
                Check(SetEndOfFile(journal_.value) != FALSE);
                break;
            }
            if (entry.crc != Crc(&entry, offsetof(Entry, crc)) || !Valid(entry, static_cast<uint64_t>(length.QuadPart))) {
                warning_ = HRESULT_FROM_WIN32(ERROR_INVALID_DATA); continue;
            }
            entries_[{entry.key, entry.edge}] = entry;
            if (entries_.size() > 600000) throw Failure{HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE)};
        }
        opened_ = true;
    }
    static bool Valid(const Entry& entry, uint64_t fileSize) {
        if (!entry.length || entry.length > 1024 * 1024 || entry.offset < sizeof(Header) ||
            entry.offset > fileSize || entry.length > fileSize - entry.offset || !std::isfinite(entry.aspect) || entry.aspect <= 0)
            return false;
        if (entry.orientation < 1 || entry.orientation > 8) return false;
        if (entry.edge == 0) return entry.codec == 0 && entry.length == 1876;
        if (entry.codec != CodecBc7 || (entry.edge != 64 && entry.edge != 128 && entry.edge != 256 && entry.edge != 512)) return false;
        uint32_t expected = 0, levels = 0;
        for (uint32_t size = entry.edge;; size /= 2) {
            if (levels >= 10 || entry.mips[levels++] != expected) return false;
            uint32_t blocks = std::max(1u, (size + 3) / 4);
            expected += blocks * blocks * 16;
            if (size == 1) break;
        }
        return entry.levels == levels && entry.mips[levels] == expected && entry.length == expected;
    }
public:
    Store(uint64_t library, std::filesystem::path root = {}) : root_(std::move(root)), library_(library) {
        if (root_.empty()) {
            PWSTR local = nullptr;
            HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local);
            if (FAILED(hr)) throw Failure{hr};
            root_ = std::filesystem::path(local) / L"Nebula" / L"Cache" / (L"Render-v" + std::to_wstring(Recipe));
            CoTaskMemFree(local);
        }
    }
    bool CanReuse(uint64_t library) {
        std::lock_guard lock(mutex_);
        return library_ == library && SUCCEEDED(openFailure_);
    }
    HRESULT Read(const ContentKey& key, uint32_t edge, Payload& payload) {
        std::lock_guard lock(mutex_);
        try {
            Open();
            auto found = entries_.find({key, edge});
            if (found == entries_.end()) return S_FALSE;
            const auto& entry = found->second;
            LARGE_INTEGER size{};
            Check(GetFileSizeEx(pack_.value, &size) != FALSE);
            if (!Valid(entry, static_cast<uint64_t>(size.QuadPart))) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            Handle mapping;
            mapping.value = CreateFileMappingW(pack_.value, nullptr, PAGE_READONLY, 0, 0, nullptr);
            Check(mapping.value != nullptr);
            SYSTEM_INFO system{}; GetSystemInfo(&system);
            uint64_t offset = entry.offset / system.dwAllocationGranularity * system.dwAllocationGranularity;
            size_t delta = static_cast<size_t>(entry.offset - offset);
            Mapping view;
            view.address = MapViewOfFile(mapping.value, FILE_MAP_READ,
                static_cast<DWORD>(offset >> 32), static_cast<DWORD>(offset), delta + entry.length);
            auto* data = static_cast<unsigned char*>(view.address);
            Check(data != nullptr);
            uint32_t crc = Crc(data + delta, entry.length);
            if (crc == entry.dataCrc) {
                payload.edge = entry.edge; payload.codec = entry.codec; payload.levels = entry.levels;
                payload.aspect = entry.aspect; payload.sourceWidth = entry.sourceWidth; payload.sourceHeight = entry.sourceHeight;
                payload.orientation = entry.orientation; payload.offsets = entry.mips;
                payload.bytes.assign(data + delta, data + delta + entry.length);
            }
            return crc == entry.dataCrc ? S_OK : HRESULT_FROM_WIN32(ERROR_CRC);
        } catch (const Failure& failure) { return Failed(failure.hr); }
          catch (const std::filesystem::filesystem_error& error) { return Failed(HRESULT_FROM_WIN32(error.code().value())); }
    }
    HRESULT Write(const ContentKey& key, const Payload& payload) {
        std::lock_guard lock(mutex_);
        try {
            Open();
            if (entries_.size() >= 600000 && !entries_.contains({key, payload.edge})) return HRESULT_FROM_WIN32(ERROR_DISK_FULL);
            LARGE_INTEGER end{};
            Check(GetFileSizeEx(pack_.value, &end) != FALSE);
            if (static_cast<uint64_t>(end.QuadPart) + payload.bytes.size() > DiskBudget) return HRESULT_FROM_WIN32(ERROR_DISK_FULL);
            Entry entry;
            entry.key = key; entry.offset = end.QuadPart; entry.edge = payload.edge; entry.codec = payload.codec;
            entry.levels = payload.levels; entry.length = static_cast<uint32_t>(payload.bytes.size()); entry.aspect = payload.aspect;
            entry.sourceWidth = payload.sourceWidth; entry.sourceHeight = payload.sourceHeight; entry.orientation = payload.orientation;
            entry.mips = payload.offsets; entry.dataCrc = Crc(payload.bytes.data(), payload.bytes.size());
            entry.crc = Crc(&entry, offsetof(Entry, crc));
            if (!Valid(entry, entry.offset + entry.length)) return E_INVALIDARG;
            Check(SetFilePointerEx(pack_.value, {}, nullptr, FILE_END) != FALSE);
            DWORD written = 0;
            Check(WriteFile(pack_.value, payload.bytes.data(), entry.length, &written, nullptr) && written == entry.length);
            Check(FlushFileBuffers(pack_.value) != FALSE);
            Check(SetFilePointerEx(journal_.value, {}, nullptr, FILE_END) != FALSE);
            Check(WriteFile(journal_.value, &entry, sizeof(entry), &written, nullptr) && written == sizeof(entry));
            Check(FlushFileBuffers(journal_.value) != FALSE);
            entries_[{key, payload.edge}] = entry;
            return S_OK;
        } catch (const Failure& failure) { return Failed(failure.hr); }
          catch (const std::filesystem::filesystem_error& error) { return Failed(HRESULT_FROM_WIN32(error.code().value())); }
    }
    HRESULT Warning() {
        std::lock_guard lock(mutex_);
        HRESULT warning = warning_; warning_ = S_OK;
        return warning;
    }
};
}
