#pragma once

#include "gfx_window_manager_api.h"
#include "gfx_host.h"

namespace Fast {
// Window backend driven by the host app through gfx_host.h (FAST3D_HOST_METAL).
// There is no SDL window: SDL only provides events, audio and controllers.
class GfxWindowBackendHost final : public GfxWindowBackend {
  public:
    GfxWindowBackendHost() = default;
    ~GfxWindowBackendHost() override = default;

    void Init(const char* gameName, const char* apiName, bool startFullScreen, uint32_t width, uint32_t height,
              int32_t posX, int32_t posY) override;
    void Close() override;
    void SetKeyboardCallbacks(bool (*onKeyDown)(int scancode), bool (*onKeyUp)(int scancode),
                              void (*onAllKeysUp)()) override;
    void SetMouseCallbacks(bool (*onMouseButtonDown)(int btn), bool (*onMouseButtonUp)(int btn)) override;
    void SetFullscreenChangedCallback(void (*onFullscreenChanged)(bool is_now_fullscreen)) override;
    void SetFullscreen(bool fullscreen) override;
    void GetActiveWindowRefreshRate(uint32_t* refreshRate) override;
    void SetCursorVisibility(bool visability) override;
    void SetMousePos(int32_t posX, int32_t posY) override;
    void GetMousePos(int32_t* x, int32_t* y) override;
    void GetMouseDelta(int32_t* x, int32_t* y) override;
    void GetMouseWheel(float* x, float* y) override;
    bool GetMouseState(uint32_t btn) override;
    void SetMouseCapture(bool capture) override;
    bool IsMouseCaptured() override;
    void GetDimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) override;
    void SetDimensions(uint32_t width, uint32_t height, int32_t posX, int32_t posY) override;
    Ship::WindowRect GetPrimaryMonitorRect() override;
    void HandleEvents() override;
    bool IsFrameReady() override;
    void SwapBuffersBegin() override;
    void SwapBuffersEnd() override;
    double GetTime() override;
    int GetTargetFps() override;
    void SetTargetFps(int fps) override;
    void SetMaxFrameLatency(int latency) override;
    const char* GetKeyName(int scancode) override;
    bool CanDisableVsync() override;
    bool IsRunning() override;
    void Destroy() override;
    bool IsFullscreen() override;

    // Current host presenter (ctx/present are null until the host sets one).
    static HPPresenter GetPresenter();
    // Host drawable size in pixels (HP_SetViewSize). GetDimensions reports points,
    // like the SDL backend on Apple platforms: pixels / GetContentScale().
    static void GetDrawableSize(uint32_t* width, uint32_t* height);
    static float GetContentScale();
    // Eyes to render per frame (HP_SetEyeCount). The SOH_STEREO_EYES environment
    // variable overrides it, for testing stereo without a stereo host.
    static int GetEyeCount();
    // Virtual screen geometry in meters (HP_SetScreenGeometry); zero while unknown.
    static void GetScreenGeometry(float* widthMeters, float* distanceMeters);

  private:
    void SyncFramerateWithTime();

    uint64_t mPreviousTime = 0;
    void (*mOnAllKeysUp)() = nullptr;
};
} // namespace Fast
