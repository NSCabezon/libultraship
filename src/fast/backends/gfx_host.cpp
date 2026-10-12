#ifdef __VISIONOS__

#include "fast/backends/gfx_host_backend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>
#include <spdlog/spdlog.h>

#include "fast/Fast3dWindow.h"
#include "fast/Fast3dGui.h"
#include "ship/Context.h"
#include "ship/config/Config.h"
#include "ship/audio/Audio.h"
#include "ship/audio/SDLAudioPlayer.h"

#define _100NANOSECONDS_IN_SECOND 10000000

namespace {
// Points per pixel of the host drawable. The engine and ImGui work in points
// (as with SDL on Apple platforms); gfx_metal renders framebuffer 0 in pixels.
// 2.0 matches the visionOS window backing scale: a 2560x1440 drawable is a
// 1280x720 pt window.
constexpr float kHostContentScale = 2.0f;

// Host state, written from the host (UI) thread and read on the game thread.
std::atomic<uint32_t> sViewWidth{ 0 };
std::atomic<uint32_t> sViewHeight{ 0 };
std::atomic<bool> sActive{ true };
std::atomic<int> sEyeCount{ 1 };
std::atomic<float> sScreenWidthMeters{ 0.0f };
std::atomic<float> sScreenDistanceMeters{ 0.0f };
// Frame stats (HP_GetStats), written by the game thread.
std::atomic<uint64_t> sStatFrames{ 0 };
std::atomic<uint64_t> sStatRunTimeNs{ 0 };
std::atomic<uint64_t> sStatRunTimeMaxNs{ 0 };
std::atomic<int> sStatEyeCount{ 1 };
// Display clock (HP_DisplayTick).
std::atomic<uint64_t> sDisplayTicks{ 0 };
std::atomic<double> sDisplayHz{ 90.0 };
std::atomic<int64_t> sDisplayTickTimeNs{ 0 }; // steady_clock time of the last tick
std::mutex sTickMutex;
std::condition_variable sTickCondition;
std::mutex sStateMutex;
std::condition_variable sActiveCondition;
HPPresenter sPresenter = { nullptr, nullptr };
} // namespace

extern "C" void HP_SetPresenter(const HPPresenter* presenter) {
    std::lock_guard<std::mutex> lock(sStateMutex);
    sPresenter = presenter != nullptr ? *presenter : HPPresenter{ nullptr, nullptr };
}

extern "C" void HP_SetViewSize(int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }
    sViewWidth = static_cast<uint32_t>(width);
    sViewHeight = static_cast<uint32_t>(height);
}

extern "C" void HP_SetActive(int active) {
    {
        std::lock_guard<std::mutex> lock(sStateMutex);
        sActive = active != 0;
    }
    sActiveCondition.notify_all();
}

extern "C" void HP_SetEyeCount(int count) {
    sEyeCount = count < 1 ? 1 : (count > 2 ? 2 : count);
}

extern "C" void HP_SetScreenGeometry(float widthMeters, float distanceMeters) {
    sScreenWidthMeters = widthMeters;
    sScreenDistanceMeters = distanceMeters;
}

static int64_t SteadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

extern "C" void HP_DisplayTick(double refreshHz) {
    // The rate the link reports is not always the rate it fires at (the simulator
    // reports 90 Hz and ticks at ~60), so measure it and snap to a standard rate.
    static double sMeasuredHz = 0.0; // main thread only
    const int64_t now = SteadyNowNs();
    const int64_t interval = now - sDisplayTickTimeNs.load(std::memory_order_relaxed);
    if (interval > 2000000 && interval < 50000000) {
        const double hz = 1e9 / static_cast<double>(interval);
        sMeasuredHz = sMeasuredHz > 0.0 ? sMeasuredHz * 0.95 + hz * 0.05 : hz;
    } else if (sMeasuredHz <= 0.0 && refreshHz >= 20.0 && refreshHz <= 240.0) {
        sMeasuredHz = refreshHz;
    }
    if (sMeasuredHz > 0.0) {
        static const double kRates[] = { 30.0, 45.0, 60.0, 72.0, 90.0, 96.0, 100.0, 120.0 };
        double best = kRates[0];
        for (double rate : kRates) {
            if (std::abs(rate - sMeasuredHz) < std::abs(best - sMeasuredHz)) {
                best = rate;
            }
        }
        sDisplayHz.store(best, std::memory_order_relaxed);
    }
    sDisplayTickTimeNs.store(now, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(sTickMutex);
        sDisplayTicks.fetch_add(1, std::memory_order_release);
    }
    sTickCondition.notify_all();
}

extern "C" void HP_GetStats(HPStats* out) {
    if (out == nullptr) {
        return;
    }
    HPStats stats = {};
    stats.size = static_cast<uint32_t>(std::min<size_t>(out->size, sizeof(HPStats)));
    stats.eyeCount = sStatEyeCount.load(std::memory_order_relaxed);
    stats.frames = sStatFrames.load(std::memory_order_acquire);
    stats.runTimeNs = sStatRunTimeNs.load(std::memory_order_relaxed);
    stats.runTimeMaxNs = sStatRunTimeMaxNs.exchange(0, std::memory_order_relaxed);
    stats.width = static_cast<int>(sViewWidth.load());
    stats.height = static_cast<int>(sViewHeight.load());
    stats.audioUnderruns = Ship::SDLAudioPlayer::GetUnderrunCount();
    auto context = Ship::Context::GetRawInstance();
    auto audio = context != nullptr ? context->GetAudio() : nullptr;
    auto player = audio != nullptr ? audio->GetAudioPlayer() : nullptr;
    stats.audioQueuedFrames = player != nullptr && player->IsInitialized() ? player->Buffered() : 0;
    memcpy(out, &stats, stats.size);
}

namespace Fast {

void GfxWindowBackendHost::RecordFrameStats(uint64_t runTimeNs) {
    sStatRunTimeNs.fetch_add(runTimeNs, std::memory_order_relaxed);
    uint64_t max = sStatRunTimeMaxNs.load(std::memory_order_relaxed);
    while (runTimeNs > max && !sStatRunTimeMaxNs.compare_exchange_weak(max, runTimeNs, std::memory_order_relaxed)) {
    }
    sStatEyeCount.store(GetEyeCount(), std::memory_order_relaxed);
    sStatFrames.fetch_add(1, std::memory_order_release);
}

HPPresenter GfxWindowBackendHost::GetPresenter() {
    std::lock_guard<std::mutex> lock(sStateMutex);
    return sPresenter;
}

void GfxWindowBackendHost::GetDrawableSize(uint32_t* width, uint32_t* height) {
    *width = sViewWidth;
    *height = sViewHeight;
}

float GfxWindowBackendHost::GetContentScale() {
    return kHostContentScale;
}

int GfxWindowBackendHost::GetEyeCount() {
    static const int sOverride = [] {
        const char* env = getenv("SOH_STEREO_EYES");
        const int count = env != nullptr ? atoi(env) : 0;
        if (count > 0) {
            SPDLOG_INFO("[HostMetal] SOH_STEREO_EYES={} overrides the host eye count", count);
        }
        return count > 0 ? std::min(count, 2) : 0;
    }();
    return sOverride > 0 ? sOverride : sEyeCount.load();
}

void GfxWindowBackendHost::GetScreenGeometry(float* widthMeters, float* distanceMeters) {
    *widthMeters = sScreenWidthMeters;
    *distanceMeters = sScreenDistanceMeters;
}

static uint32_t PixelsToPoints(uint32_t pixels) {
    const uint32_t points = static_cast<uint32_t>(pixels / kHostContentScale);
    return points > 0 ? points : 1;
}

void GfxWindowBackendHost::Init(const char* gameName, const char* apiName, bool startFullScreen, uint32_t width,
                                uint32_t height, int32_t posX, int32_t posY) {
    mFullScreen = false;
    mOnFullscreenChanged = nullptr;
    mOnKeyDown = nullptr;
    mOnKeyUp = nullptr;
    mOnMouseButtonDown = nullptr;
    mOnMouseButtonUp = nullptr;

    // Until the host reports its drawable, use the configured size.
    uint32_t expected = 0;
    sViewWidth.compare_exchange_strong(expected, width);
    expected = 0;
    sViewHeight.compare_exchange_strong(expected, height);

    // No video subsystem: SDL only provides events, audio and controllers.
    SDL_InitSubSystem(SDL_INIT_EVENTS);

    SPDLOG_INFO("[HostMetal] window backend init: drawable {}x{} px, scale {}, presenter {}", sViewWidth.load(),
                sViewHeight.load(), kHostContentScale, GetPresenter().present != nullptr ? "set" : "not set");

    // Fast3dGui creates the Metal device (MetalInitHost) and the ImGui platform layer.
    Fast::GuiWindowInitData windowImpl = {};
    windowImpl.Backend = WindowBackend::FAST3D_HOST_METAL;
    std::dynamic_pointer_cast<Fast::Fast3dGui>(Ship::Context::GetRawInstance()->GetWindow()->GetGui())
        ->Init(windowImpl);
}

void GfxWindowBackendHost::Close() {
    mIsRunning = false;
}

void GfxWindowBackendHost::SetKeyboardCallbacks(bool (*onKeyDown)(int scancode), bool (*onKeyUp)(int scancode),
                                                void (*onAllKeysUp)()) {
    mOnKeyDown = onKeyDown;
    mOnKeyUp = onKeyUp;
    mOnAllKeysUp = onAllKeysUp;
}

void GfxWindowBackendHost::SetMouseCallbacks(bool (*onMouseButtonDown)(int btn), bool (*onMouseButtonUp)(int btn)) {
    mOnMouseButtonDown = onMouseButtonDown;
    mOnMouseButtonUp = onMouseButtonUp;
}

void GfxWindowBackendHost::SetFullscreenChangedCallback(void (*onFullscreenChanged)(bool is_now_fullscreen)) {
    mOnFullscreenChanged = onFullscreenChanged;
}

void GfxWindowBackendHost::SetFullscreen(bool fullscreen) {
    // The host scene owns presentation; there is no fullscreen mode.
    mFullScreen = false;
}

void GfxWindowBackendHost::GetActiveWindowRefreshRate(uint32_t* refreshRate) {
    *refreshRate = 90;
}

void GfxWindowBackendHost::SetCursorVisibility(bool visability) {
}

void GfxWindowBackendHost::SetMousePos(int32_t posX, int32_t posY) {
}

void GfxWindowBackendHost::GetMousePos(int32_t* x, int32_t* y) {
    *x = 0;
    *y = 0;
}

void GfxWindowBackendHost::GetMouseDelta(int32_t* x, int32_t* y) {
    *x = 0;
    *y = 0;
}

void GfxWindowBackendHost::GetMouseWheel(float* x, float* y) {
    *x = 0.0f;
    *y = 0.0f;
}

bool GfxWindowBackendHost::GetMouseState(uint32_t btn) {
    return false;
}

void GfxWindowBackendHost::SetMouseCapture(bool capture) {
}

bool GfxWindowBackendHost::IsMouseCaptured() {
    return false;
}

void GfxWindowBackendHost::GetDimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) {
    // Points, like SDL_GetWindowSize on Apple platforms (see GetDrawableSize for pixels).
    *width = PixelsToPoints(sViewWidth);
    *height = PixelsToPoints(sViewHeight);
    *posX = 0;
    *posY = 0;
}

void GfxWindowBackendHost::SetDimensions(uint32_t width, uint32_t height, int32_t posX, int32_t posY) {
    // The host decides the drawable size (HP_SetViewSize).
}

Ship::WindowRect GfxWindowBackendHost::GetPrimaryMonitorRect() {
    return { 0, 0, static_cast<int32_t>(PixelsToPoints(sViewWidth)), static_cast<int32_t>(PixelsToPoints(sViewHeight)) };
}

void GfxWindowBackendHost::HandleEvents() {
    // Controller device events stay queued for the ControlDeck; everything else
    // is drained here (TODO(visionOS WP4): feed ImGui).
    SDL_Event event;
    SDL_PumpEvents();
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_CONTROLLERDEVICEADDED - 1) > 0 ||
           SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_CONTROLLERDEVICEREMAPPED + 1, SDL_LASTEVENT) > 0) {
        switch (event.type) {
            case SDL_APP_WILLENTERBACKGROUND:
            case SDL_APP_TERMINATING: {
                auto context = Ship::Context::GetRawInstance();
                if (context->GetConfig() != nullptr) {
                    context->GetConfig()->Save();
                }
                break;
            }
            case SDL_QUIT:
                Close();
                break;
            default:
                break;
        }
    }
}

bool GfxWindowBackendHost::IsFrameReady() {
    if (sActive) {
        return true;
    }
    // Inactive scene: park the game thread instead of spinning in its frame loop.
    std::unique_lock<std::mutex> lock(sStateMutex);
    sActiveCondition.wait_for(lock, std::chrono::milliseconds(100), [] { return sActive.load(); });
    return sActive;
}

static uint64_t qpc_to_100ns(uint64_t qpc) {
    const uint64_t qpc_freq = SDL_GetPerformanceFrequency();
    return qpc / qpc_freq * _100NANOSECONDS_IN_SECOND + qpc % qpc_freq * _100NANOSECONDS_IN_SECOND / qpc_freq;
}

void GfxWindowBackendHost::SyncFramerateWithTime() {
    uint64_t t = qpc_to_100ns(SDL_GetPerformanceCounter());

    const int64_t next = mPreviousTime + 10 * 1000000 / mTargetFps;
    int64_t left = next - t;
    // Leave the scheduler some slack, as on the other Apple backends.
    left -= 10000UL;
    if (left > 0) {
        const timespec spec = { 0, left * 100 };
        nanosleep(&spec, nullptr);
    }

    t = qpc_to_100ns(SDL_GetPerformanceCounter());
    if (left > 0 && t - next < 10000) {
        t = next;
    }
    mPreviousTime = t;
}

// Paces on the host's display ticks: one frame every refreshHz / targetFps ticks (a
// fraction, e.g. 4.5 at 20 fps on 90 Hz, alternates 4 and 5). A late frame does not
// try to catch up. Returns false while no ticks arrive (no host clock): use the timer.
bool GfxWindowBackendHost::SyncFramerateWithDisplay() {
    constexpr int64_t kStaleNs = 100 * 1000 * 1000;
    if (SteadyNowNs() - sDisplayTickTimeNs.load(std::memory_order_relaxed) > kStaleNs) {
        mDueTick = 0.0;
        return false;
    }
    const double ticksPerFrame = std::max(1.0, sDisplayHz.load(std::memory_order_relaxed) / std::max(1u, mTargetFps));
    const double now = static_cast<double>(sDisplayTicks.load(std::memory_order_acquire));
    mDueTick = (mDueTick <= 0.0 || mDueTick + ticksPerFrame < now) ? now + 1.0 : mDueTick + ticksPerFrame;
    const uint64_t due = static_cast<uint64_t>(mDueTick + 0.5 - 1e-6);
    std::unique_lock<std::mutex> lock(sTickMutex);
    sTickCondition.wait_for(lock, std::chrono::milliseconds(100),
                            [due] { return sDisplayTicks.load(std::memory_order_acquire) >= due; });
    return true;
}

void GfxWindowBackendHost::SwapBuffersBegin() {
    if (!SyncFramerateWithDisplay()) {
        SyncFramerateWithTime();
    }
}

void GfxWindowBackendHost::SwapBuffersEnd() {
}

double GfxWindowBackendHost::GetTime() {
    return 0.0;
}

int GfxWindowBackendHost::GetTargetFps() {
    return mTargetFps;
}

void GfxWindowBackendHost::SetTargetFps(int fps) {
    mTargetFps = fps;
}

void GfxWindowBackendHost::SetMaxFrameLatency(int latency) {
}

const char* GfxWindowBackendHost::GetKeyName(int scancode) {
    return "";
}

bool GfxWindowBackendHost::CanDisableVsync() {
    return false;
}

bool GfxWindowBackendHost::IsRunning() {
    return mIsRunning;
}

void GfxWindowBackendHost::Destroy() {
    SDL_Quit();
}

bool GfxWindowBackendHost::IsFullscreen() {
    return false;
}
} // namespace Fast
#endif
