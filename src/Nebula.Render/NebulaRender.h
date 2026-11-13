#pragma once

#include <stdint.h>
#include <stddef.h>
#include <wchar.h>

#ifdef NEBULA_RENDER_EXPORTS
#define NB_API __declspec(dllexport)
#else
#define NB_API __declspec(dllimport)
#endif

typedef struct NbItem {
    uint32_t id;
    float x, y, z, aspect, r, g, b;
    const wchar_t* imagePath;
} NbItem;

typedef struct NbItemV2 {
    NbItem item;
    const wchar_t* originalPath;
    uint64_t contentVersion;
} NbItemV2;

// Set size=64. States: 0 none, 1 loading, 2 intermediate, 3 full original,
// 4 original-pixel ROI, 5 failed. ROI coordinates are in oriented source pixels.
typedef struct NbFocusStats {
    uint32_t size;
    int32_t id;
    uint32_t sourceWidth, sourceHeight, textureWidth, textureHeight;
    uint32_t roiX, roiY, roiWidth, roiHeight;
    uint32_t state;
    float decodeMs;
    uint64_t gpuBytes;
    int32_t error;
    uint32_t pending;
} NbFocusStats;

// Set size=56. CPU stages are elapsed wall time, including parallel-worker waits.
// Reserved currently contains diagnostic motion flags, not a stable control API.
typedef struct NbFrameDiagnostics {
    uint32_t size, reserved;
    float prepareMs, orderMs, uploadMs, submitMs;
    uint64_t instanceUploadBytes, densityUploadBytes, tiles, dots;
} NbFrameDiagnostics;

typedef struct NbStats {
    uint32_t size;
    uint32_t items;
    uint32_t resident;
    uint32_t pending;
    uint32_t failed; // Unique photos with an unrecoverable decode failure in this scene.
    float fps;
    uint64_t frames;
} NbStats;

typedef struct NbGlassRect {
    float x, y, width, height, radius, tint;
} NbGlassRect;

typedef struct NbGlassPanel {
    float x, y, width, height, radius, tint, opacity;
} NbGlassPanel;

typedef struct NbResult {
    uint32_t id;
    float x, y, z, weight;
} NbResult;

typedef struct NbLine {
    uint32_t a, b;
    float weight;
} NbLine;

// Set size=88. Snapshot of the last successfully presented, animated camera.
// viewProj is ROW-MAJOR storage for COLUMN-VECTOR multiplication:
// clip[row] = sum(viewProj[row*4+column] * float4(x,y,z,1)[column]).
// Drawable pixels: x=(clip.x/clip.w+1)*width/2, y=(1-clip.y/clip.w)*height/2.
// The camera matrix excludes per-photo presentation-layer depth adjustments.
// S_FALSE means no frame is available yet; width/height are then zero.
typedef struct NbView {
    uint32_t size;
    float viewProj[16];
    float width, height;
    float scaleX, scaleY;
    float pixelsPerUnitAtDistance1;
} NbView;

// Set size=128. Tier counts cover visible continuous-footprint bands; fully faded
// photos are excluded. Hysteresis/dwell affect requests, not these bands. MosaicReady is
// the completed scene-resident hierarchy count; MosaicPending counts unfinished
// records. Pending counts bounded decode jobs (one mosaic job holds up to 64 records).
// CPU time includes preparation/uploads; GPU timestamps cover rendering passes
// and lag behind Frames (see GpuFrame); FrameMs runs through Present. GpuBytes is
// nominal resource size, not driver allocation accounting. Counters are cumulative.
// Failed counts unique photos (including original failures), not renderer faults.
// Recovered metadata/profile errors and cancelled jobs do not increment it.
typedef struct NbLodStats {
    uint32_t size, items, visible, dots, mosaics, thumbnails, previews, mosaicReady;
    uint32_t thumbnailResident, previewResident, pending, failed;
    float cap0, cap1, cap2, cpuMs, gpuMs, frameMs, pickMs;
    uint32_t mosaicPending;
    uint64_t frames, gpuFrame, flips, uploads, evictions, gpuBytes;
} NbLodStats;

#ifdef __cplusplus
static_assert(sizeof(void*) == 8, "NebulaRender supports ARM64 and x64.");
static_assert(sizeof(NbItem) == 40 && offsetof(NbItem, imagePath) == 32);
static_assert(sizeof(NbItemV2) == 56);
static_assert(sizeof(NbFocusStats) == 64);
static_assert(sizeof(NbFrameDiagnostics) == 56 && offsetof(NbFrameDiagnostics, instanceUploadBytes) == 24);
static_assert(sizeof(NbStats) == 32 && offsetof(NbStats, frames) == 24);
static_assert(sizeof(NbGlassRect) == 24);
static_assert(sizeof(NbGlassPanel) == 28 && offsetof(NbGlassPanel, opacity) == 24);
static_assert(sizeof(NbResult) == 20 && offsetof(NbResult, weight) == 16);
static_assert(sizeof(NbLine) == 12);
static_assert(sizeof(NbView) == 88 && offsetof(NbView, viewProj) == 4);
static_assert(sizeof(NbLodStats) == 128);
extern "C" {
#endif

NB_API int nb_create(void* panelInspectable, void** renderer);
NB_API void nb_destroy(void* renderer);
NB_API int nb_resize(void* renderer, int widthPixels, int heightPixels, float scaleX, float scaleY);
NB_API int nb_set_scene(void* renderer, const NbItem* items, int count);
NB_API int nb_set_positions(void* renderer, const float* xyz, int count);
NB_API int nb_set_filter(void* renderer, const unsigned char* mask, int count);
NB_API int nb_select(void* renderer, int id);
NB_API int nb_pick(void* renderer, float xPixels, float yPixels, int* id);
NB_API int nb_orbit(void* renderer, float dx, float dy);
NB_API int nb_pan(void* renderer, float dx, float dy);
NB_API int nb_zoom(void* renderer, float delta);
NB_API int nb_fit(void* renderer);
NB_API int nb_set_theme(void* renderer, int light);
NB_API int nb_get_stats(void* renderer, NbStats* stats);
NB_API int nb_get_error(void* renderer, wchar_t* buffer, int capacity);
NB_API int nb_set_glass(void* renderer, const NbGlassRect* rects, int count);
NB_API int nb_set_glass_panels(void* renderer, const NbGlassPanel* panels, int count);
NB_API int nb_hover(void* renderer, int id);
NB_API int nb_focus(void* renderer, int id);
NB_API int nb_back(void* renderer);
NB_API int nb_set_reduced_motion(void* renderer, int enabled);
NB_API int nb_set_pixels(void* renderer, int level);
NB_API int nb_set_results(void* renderer, const NbResult* results, int count);
NB_API int nb_clear_results(void* renderer);
NB_API int nb_set_highlight(void* renderer, const uint32_t* ids, const float* weights, int count);
NB_API int nb_set_lines(void* renderer, const NbLine* lines, int count);
NB_API int nb_focus_set(void* renderer, const uint32_t* ids, int count);
NB_API int nb_get_view(void* renderer, NbView* view);
NB_API int nb_pick_lod(void* renderer, float xPixels, float yPixels, int* id, int* tier);
NB_API int nb_get_lod_stats(void* renderer, NbLodStats* stats);
// Both UTF-16 source paths are copied synchronously, with no file I/O on the caller.
NB_API int nb_set_scene_v2(void* renderer, const NbItemV2* items, int count);
NB_API int nb_get_focus_stats(void* renderer, NbFocusStats* stats);
NB_API int nb_set_depth_options(void* renderer, int atmosphere, int focusDof);
NB_API int nb_get_frame_diagnostics(void* renderer, NbFrameDiagnostics* diagnostics);

#ifdef __cplusplus
}
#endif
