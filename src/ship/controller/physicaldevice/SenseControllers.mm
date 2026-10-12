#include "ship/controller/physicaldevice/SenseControllers.h"

#import <GameController/GameController.h>
#include <SDL2/SDL.h>
#include <spdlog/spdlog.h>
#include <cstring>

// See SenseControllers.h. Requires SDL's virtual joystick driver (enabled for visionOS in
// cmake/dependencies/visionos.cmake).

namespace Ship::SenseControllers {
namespace {
constexpr int kLeft = 0;
constexpr int kRight = 1;

// Virtual layout: every standard button the pair has, plus all six axes. SDL numbers the
// virtual buttons in SDL_GameControllerButton order over this mask (VIRTUAL_JoystickGetGamepadMapping).
constexpr Uint32 kButtonMask = (1u << SDL_CONTROLLER_BUTTON_A) | (1u << SDL_CONTROLLER_BUTTON_B) |
                               (1u << SDL_CONTROLLER_BUTTON_X) | (1u << SDL_CONTROLLER_BUTTON_Y) |
                               (1u << SDL_CONTROLLER_BUTTON_BACK) | (1u << SDL_CONTROLLER_BUTTON_START) |
                               (1u << SDL_CONTROLLER_BUTTON_LEFTSTICK) | (1u << SDL_CONTROLLER_BUTTON_RIGHTSTICK) |
                               (1u << SDL_CONTROLLER_BUTTON_LEFTSHOULDER) |
                               (1u << SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);

SDL_Joystick* sJoystick = nullptr;
bool sPresent[2] = { false, false };
bool sAttachFailed = false; // retried only when the connected set changes

// The hand of a single-hand Sense controller from its name ("PlayStation VR2 Sense Controller (L)"),
// or -1. A combined "(L/R)" device is left to SDL.
int SenseHand(const char* name) {
    static const char kPrefix[] = "PlayStation VR2 Sense Controller";
    if (name == nullptr || std::strncmp(name, kPrefix, sizeof(kPrefix) - 1) != 0) {
        return -1;
    }
    const size_t length = std::strlen(name);
    if (length >= 3 && std::strcmp(name + length - 3, "(L)") == 0) {
        return kLeft;
    }
    if (length >= 3 && std::strcmp(name + length - 3, "(R)") == 0) {
        return kRight;
    }
    return -1;
}

struct HandState {
    float StickX = 0.0f;
    float StickY = 0.0f; // GameController convention: up is positive
    float Trigger = 0.0f;
    bool StickButton = false;
    bool Grip = false;
    bool Lower = false; // "Button A": Square (L) / Cross (R)
    bool Upper = false; // "Button B": Triangle (L) / Circle (R)
    bool Menu = false;  // Create (L) / Options (R)
};

bool Pressed(GCPhysicalInputProfile* profile, NSString* name) {
    GCControllerButtonInput* button = profile.buttons[name];
    return button != nil && button.isPressed;
}

float Value(GCPhysicalInputProfile* profile, NSString* name) {
    GCControllerButtonInput* button = profile.buttons[name];
    return button != nil ? button.value : 0.0f;
}

// The Sense publishes its stick several ways and not every one carries the live value
// (KleptonControllers.swift, observed on device): take the first one that is deflected.
void ReadStick(GCPhysicalInputProfile* profile, HandState& state) {
    auto live = [](float x, float y) { return x * x + y * y > 0.02f * 0.02f; };
    for (NSString* name in @[ @"Thumbstick", GCInputLeftThumbstick, GCInputRightThumbstick ]) {
        GCControllerDirectionPad* pad = profile.dpads[name];
        if (pad != nil && live(pad.xAxis.value, pad.yAxis.value)) {
            state.StickX = pad.xAxis.value;
            state.StickY = pad.yAxis.value;
            return;
        }
    }
    GCControllerAxisInput* x = profile.axes[@"Thumbstick X Axis"];
    GCControllerAxisInput* y = profile.axes[@"Thumbstick Y Axis"];
    if (x != nil && y != nil && live(x.value, y.value)) {
        state.StickX = x.value;
        state.StickY = y.value;
        return;
    }
    state.StickX = Value(profile, @"Thumbstick Right") - Value(profile, @"Thumbstick Left");
    state.StickY = Value(profile, @"Thumbstick Up") - Value(profile, @"Thumbstick Down");
}

HandState ReadHand(GCController* controller) {
    HandState state;
    GCPhysicalInputProfile* profile = controller.physicalInputProfile;
    ReadStick(profile, state);
    state.Trigger = Value(profile, @"Trigger");
    state.StickButton = Pressed(profile, @"Thumbstick Button");
    state.Grip = Pressed(profile, @"Grip");
    state.Lower = Pressed(profile, GCInputButtonA);
    state.Upper = Pressed(profile, GCInputButtonB);
    state.Menu = Pressed(profile, GCInputButtonMenu) || Pressed(profile, GCInputButtonOptions) ||
                 Pressed(profile, @"Button Share");
    return state;
}

Sint16 StickAxis(float v) {
    v = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
    return (Sint16)(v * 32767.0f);
}

// A virtual trigger axis rests at the minimum; SDL maps the full range to 0..32767.
Sint16 TriggerAxis(float v) {
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return (Sint16)(-32768.0f + v * 65535.0f);
}

void SetButton(SDL_GameControllerButton button, bool pressed) {
    // Index of `button` among the virtual buttons: the mask bits below it.
    const int index = __builtin_popcount(kButtonMask & ((1u << button) - 1u));
    SDL_JoystickSetVirtualButton(sJoystick, index, pressed ? SDL_PRESSED : SDL_RELEASED);
}

// Called by SDL whenever it updates the virtual joystick (every SDL_PumpEvents), so the
// ControlDeck and ImGui read the controllers' current state.
void SDLCALL UpdateVirtual(void* /*userdata*/) {
    if (sJoystick == nullptr) {
        return;
    }
    HandState hands[2];
    bool present[2] = { false, false };
    @autoreleasepool {
        for (GCController* controller in GCController.controllers) {
            const int hand = SenseHand(controller.vendorName.UTF8String);
            if (hand >= 0 && !present[hand]) {
                present[hand] = true;
                hands[hand] = ReadHand(controller);
            }
        }
    }

    const HandState none;
    const bool pair = present[kLeft] && present[kRight];
    // One controller alone plays as a whole pad: its stick is the left stick.
    const HandState& stickLeft = present[kLeft] ? hands[kLeft] : (present[kRight] ? hands[kRight] : none);
    const HandState& stickRight = pair ? hands[kRight] : none;
    const HandState& left = present[kLeft] ? hands[kLeft] : none;
    const HandState& right = present[kRight] ? hands[kRight] : none;

    SDL_JoystickSetVirtualAxis(sJoystick, SDL_CONTROLLER_AXIS_LEFTX, StickAxis(stickLeft.StickX));
    SDL_JoystickSetVirtualAxis(sJoystick, SDL_CONTROLLER_AXIS_LEFTY, StickAxis(-stickLeft.StickY));
    SDL_JoystickSetVirtualAxis(sJoystick, SDL_CONTROLLER_AXIS_RIGHTX, StickAxis(stickRight.StickX));
    SDL_JoystickSetVirtualAxis(sJoystick, SDL_CONTROLLER_AXIS_RIGHTY, StickAxis(-stickRight.StickY));
    SDL_JoystickSetVirtualAxis(sJoystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT, TriggerAxis(left.Trigger));
    SDL_JoystickSetVirtualAxis(sJoystick, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, TriggerAxis(right.Trigger));

    SetButton(SDL_CONTROLLER_BUTTON_LEFTSTICK, stickLeft.StickButton);
    SetButton(SDL_CONTROLLER_BUTTON_RIGHTSTICK, stickRight.StickButton);
    SetButton(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, left.Grip);
    SetButton(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, right.Grip);
    SetButton(SDL_CONTROLLER_BUTTON_BACK, left.Menu);
    SetButton(SDL_CONTROLLER_BUTTON_START, right.Menu);
    if (pair) {
        SetButton(SDL_CONTROLLER_BUTTON_A, right.Lower);
        SetButton(SDL_CONTROLLER_BUTTON_B, right.Upper);
        SetButton(SDL_CONTROLLER_BUTTON_X, left.Lower);
        SetButton(SDL_CONTROLLER_BUTTON_Y, left.Upper);
    } else {
        // A lone controller needs accept/back: lower = A (Cross), upper = B (Circle).
        SetButton(SDL_CONTROLLER_BUTTON_A, stickLeft.Lower);
        SetButton(SDL_CONTROLLER_BUTTON_B, stickLeft.Upper);
        SetButton(SDL_CONTROLLER_BUTTON_X, false);
        SetButton(SDL_CONTROLLER_BUTTON_Y, false);
    }
}

void Attach() {
    SDL_VirtualJoystickDesc desc;
    SDL_zero(desc);
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = __builtin_popcount(kButtonMask);
    desc.button_mask = kButtonMask;
    desc.name = "PS VR2 Sense Pair";
    desc.Update = UpdateVirtual;

    const int deviceIndex = SDL_JoystickAttachVirtualEx(&desc);
    if (deviceIndex < 0) {
        SPDLOG_ERROR("[Sense] could not attach the virtual controller: {}", SDL_GetError());
        sAttachFailed = true;
        return;
    }
    // Kept open so SDL keeps calling UpdateVirtual even before the ControlDeck opens it.
    sJoystick = SDL_JoystickOpen(deviceIndex);
    if (sJoystick == nullptr) {
        SPDLOG_ERROR("[Sense] could not open the virtual controller: {}", SDL_GetError());
        SDL_JoystickDetachVirtual(deviceIndex);
        sAttachFailed = true;
        return;
    }
    SPDLOG_INFO("[Sense] virtual controller attached as instance {}", SDL_JoystickInstanceID(sJoystick));
}

void Detach() {
    const SDL_JoystickID instanceId = SDL_JoystickInstanceID(sJoystick);
    SDL_JoystickClose(sJoystick);
    sJoystick = nullptr;
    for (int n = 0; n < SDL_NumJoysticks(); n++) {
        if (SDL_JoystickGetDeviceInstanceID(n) == instanceId) {
            SDL_JoystickDetachVirtual(n);
            break;
        }
    }
    SPDLOG_INFO("[Sense] virtual controller detached (instance {})", instanceId);
}
} // namespace

void Update() {
    bool present[2] = { false, false };
    @autoreleasepool {
        for (GCController* controller in GCController.controllers) {
            const int hand = SenseHand(controller.vendorName.UTF8String);
            if (hand >= 0) {
                present[hand] = true;
            }
        }
    }
    if (present[kLeft] != sPresent[kLeft] || present[kRight] != sPresent[kRight]) {
        sPresent[kLeft] = present[kLeft];
        sPresent[kRight] = present[kRight];
        sAttachFailed = false;
        SPDLOG_INFO("[Sense] controllers connected: left {}, right {}", present[kLeft], present[kRight]);
        @autoreleasepool {
            for (GCController* controller in GCController.controllers) {
                SPDLOG_INFO("[Sense] visible controller: '{}' category '{}' extendedGamepad {}",
                            controller.vendorName.UTF8String ?: "?", controller.productCategory.UTF8String ?: "?",
                            controller.extendedGamepad != nil);
            }
        }
    }

    const bool wanted = present[kLeft] || present[kRight];
    if (wanted && sJoystick == nullptr && !sAttachFailed) {
        Attach();
    } else if (!wanted && sJoystick != nullptr) {
        Detach();
    }
}

bool IsReplacedDevice(int sdlDeviceIndex) {
    return SenseHand(SDL_JoystickNameForIndex(sdlDeviceIndex)) >= 0;
}

} // namespace Ship::SenseControllers
