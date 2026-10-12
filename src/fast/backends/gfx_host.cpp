#ifdef __VISIONOS__

#include "fast/backends/gfx_host_backend.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <time.h>

#include <SDL.h>

#include "fast/Fast3dWindow.h"
#include "fast/Fast3dGui.h"
#include "ship/Context.h"
#include "ship/config/Config.h"

#define _100NANOSECONDS_IN_SECOND 10000000

namespace {
// Host state, written from the host (UI) thread and read on the game thread.
std::atomic<uint32_t> sViewWidth{ 0 };
std::atomic<uint32_t> sViewHeight{ 0 };
std::atomic<bool> sActive{ true };
std::atomic<int> sEyeCount{ 1 };
std::atomic<float> sScreenWidthMeters{ 0.0f };
std::atomic<float> sScreenDistanceMeters{ 0.0f };
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

// TODO(visionOS stereo): stored only; consumed by the Metal/stereo work.
extern "C" void HP_SetEyeCount(int count) {
    sEyeCount = count < 1 ? 1 : (count > 2 ? 2 : count);
}

extern "C" void HP_SetScreenGeometry(float widthMeters, float distanceMeters) {
    sScreenWidthMeters = widthMeters;
    sScreenDistanceMeters = distanceMeters;
}

namespace Fast {

HPPresenter GfxWindowBackendHost::GetPresenter() {
    std::lock_guard<std::mutex> lock(sStateMutex);
    return sPresenter;
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

    // TODO(visionOS WP3/WP4): Metal device from the host and ImGui platform layer.
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
    *width = sViewWidth;
    *height = sViewHeight;
    *posX = 0;
    *posY = 0;
}

void GfxWindowBackendHost::SetDimensions(uint32_t width, uint32_t height, int32_t posX, int32_t posY) {
    // The host decides the drawable size (HP_SetViewSize).
}

Ship::WindowRect GfxWindowBackendHost::GetPrimaryMonitorRect() {
    return { 0, 0, static_cast<int32_t>(sViewWidth.load()), static_cast<int32_t>(sViewHeight.load()) };
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

void GfxWindowBackendHost::SwapBuffersBegin() {
    SyncFramerateWithTime();
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
