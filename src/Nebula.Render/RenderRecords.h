#pragma once
#include <DirectXMath.h>
#include <array>
#include <cstdint>

namespace nebula_records {
struct Tile {
    DirectX::XMFLOAT3 position;
    float halfX;
    uint32_t color;
    float halfY, aspect, alpha;
    DirectX::XMFLOAT4 effects; // selection, hover, highlight, contrast
    std::array<uint32_t, 4> pages; // mosaic ID, source A token, source B token, flags
    DirectX::XMFLOAT4 state; // preview residency mix, eased edge, presentation lift, near visibility
    DirectX::XMFLOAT4 content{0,1,1,1}; // mosaic level, thumbnail weight, preview weight, mosaic residency
};
static_assert(sizeof(Tile) == 96);
inline uint32_t Flags(int repA, int repB, int pixels, bool light, bool original, bool art = true, int thumbnail = -1) {
    return static_cast<uint32_t>(repA | (repB << 3) | (pixels << 6) | (light ? 512 : 0) | (original ? 1024 : 0) | (art ? 2048 : 0)) |
           (static_cast<uint32_t>(thumbnail < 0 ? 65535 : thumbnail) << 16);
}
}
