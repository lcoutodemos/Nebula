#pragma once

#include <cstddef>
#include <cstdint>
#include <array>

namespace nebula_limits {
inline constexpr std::size_t MaxItems = 100000;
inline constexpr std::size_t ThumbnailSlots = 8192;
inline constexpr std::size_t PreviewSlots = 1024 + 512 + 128;
inline constexpr std::size_t PoolSlots = ThumbnailSlots + PreviewSlots;
inline constexpr std::uint32_t ThumbnailEdge = 64;
inline constexpr std::uint32_t ThumbEdge = 256;
// Complete BC7 mip chains: 156,850,176 bytes total; the focused original has
// an independent 96 MiB reserve in OriginalLoader.h.
inline constexpr std::array<std::uint32_t, 4> MipEdges{64,128,256,512};
inline constexpr std::array<std::uint32_t, 4> MipSlots{8192,1024,512,128};
inline constexpr std::size_t Bc7ChainBytes(std::uint32_t edge) {
    std::size_t bytes = 0;
    for (;;) {
        auto blocks = (edge + 3) / 4;
        bytes += blocks * blocks * 16;
        if (edge == 1) return bytes;
        edge /= 2;
    }
}
inline constexpr std::size_t RequestLimit = 256;
inline constexpr std::size_t ResultLimit = 16;
inline constexpr std::size_t MosaicBatch = 64;
inline constexpr std::size_t MosaicJobs = 8;
inline constexpr std::size_t RequestsPerFrame = 40;
inline constexpr std::size_t UploadBudget = 8 * 1024 * 1024;
inline constexpr std::size_t PathBudget = 64 * 1024 * 1024;
inline constexpr std::size_t ImageBytesPerSlice = static_cast<std::size_t>(ThumbEdge) * ThumbEdge * 4;
inline constexpr std::size_t MosaicBytesPerSlice = 1876;
inline constexpr std::size_t ResidentMosaicBytes = 1364;
// Normal hierarchy: 136.4 MB at 100K. Pixels art adds 51.2 MB only on demand.
inline constexpr std::size_t ArtBytesPerSlice = 512;
inline constexpr std::size_t MosaicChunkItems = 32768;
inline constexpr std::size_t MosaicChunks = (MaxItems + MosaicChunkItems - 1) / MosaicChunkItems;
inline constexpr std::size_t ImagePoolBytes = MipSlots[0] * Bc7ChainBytes(64) + MipSlots[1] * Bc7ChainBytes(128) +
    MipSlots[2] * Bc7ChainBytes(256) + MipSlots[3] * Bc7ChainBytes(512);
inline constexpr std::size_t MosaicPoolBytes = MaxItems * ResidentMosaicBytes;
inline constexpr std::size_t DecodedBytesPerItem = ImageBytesPerSlice + MosaicBytesPerSlice;
inline constexpr std::uint64_t FiveThousandMemoryBudget = 600000000;
inline constexpr std::uint64_t HundredThousandMemoryBudget = 1200000000;
}
