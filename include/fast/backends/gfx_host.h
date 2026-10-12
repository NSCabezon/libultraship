#pragma once

/*
 * Host-driven window backend (FAST3D_HOST_METAL) used on visionOS.
 *
 * The host app (Swift/ObjC) owns the view and the compositor. It tells the
 * engine the drawable size and whether the scene is active, and supplies a
 * presenter that encodes the presentation of each finished frame. Plain C with
 * opaque pointers so it can be called from Swift through a bridging header.
 * All functions are safe to call from any thread.
 *
 * Notes for the Metal side: the engine renders one output texture per eye
 * (not a single shared framebuffer 0), and the host must not assume exactly
 * one present() call per frame (one per eye, possibly none for a dropped frame).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Grows by appending fields only, so existing hosts keep working. */
typedef struct HPPresentInfo {
    void* texture;       /* id<MTLTexture>, output for this eye */
    void* commandBuffer; /* id<MTLCommandBuffer> to encode the presentation into */
    int eye;             /* 0 = left/mono, 1 = right */
    int eyeCount;        /* 1 today */
    uint64_t frameIndex;
    int width, height; /* pixels */
} HPPresentInfo;

typedef struct HPPresenter {
    /* Opaque host pointer, passed back unchanged to present(). */
    void* ctx;
    /* Called on the game thread before the engine commits its command buffer. */
    void (*present)(void* ctx, const HPPresentInfo* info);
} HPPresenter;

/* Installs (or clears, with NULL) the presenter. The struct is copied. */
void HP_SetPresenter(const HPPresenter* presenter);

/* Drawable size in pixels. Zero sizes are ignored. */
void HP_SetViewSize(int width, int height);

/* Scene activity. While inactive the engine skips frames. */
void HP_SetActive(int active);

/* Number of eyes to render (1 = mono, 2 = stereo). Values are clamped to 1..2. */
void HP_SetEyeCount(int count);

/* Virtual screen width and distance from the viewer, in meters. */
void HP_SetScreenGeometry(float widthMeters, float distanceMeters);

/* Display clock: the host calls this on every display refresh (CADisplayLink, main
 * thread) with the nominal display rate in Hz (a hint: the engine measures the tick
 * rate and snaps it to a standard one). While ticks arrive the engine starts each frame
 * on a tick, every rate/targetFps ticks (2 for 45 fps on a 90 Hz panel), instead of
 * using its own timer; without ticks for 100 ms it falls back to the timer. */
void HP_DisplayTick(double refreshHz);

/* Engine counters for the host's FPS meter and perf reports. Grows by appending
 * fields only: the host sets `size` to sizeof(HPStats) as it was compiled and the
 * engine fills at most that many bytes (and writes back the size it filled). */
typedef struct HPStats {
    uint32_t size;         /* in: sizeof(HPStats) on the host; out: bytes filled */
    int eyeCount;          /* eyes rendered by the last frame */
    uint64_t frames;       /* frames run by the interpreter (monotonic) */
    uint64_t runTimeNs;    /* CPU time in Interpreter::Run, all eyes, summed over `frames` (monotonic) */
    uint64_t runTimeMaxNs; /* longest frame since the previous HP_GetStats call (the call resets it) */
    int width, height;     /* engine render size in pixels (HP_SetViewSize) */
    uint64_t audioUnderruns; /* times the audio queue ran dry (SDL backend; monotonic) */
    int audioQueuedFrames;   /* audio frames queued in the SDL device right now */
} HPStats;

/* Snapshot of the engine counters. Lock-free; meant for a single polling consumer
 * (runTimeMaxNs is reset on each call). */
void HP_GetStats(HPStats* out);

#ifdef __cplusplus
}
#endif
