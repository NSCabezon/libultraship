#include "ship/controller/physicaldevice/SDLAddRemoveDeviceEventHandler.h"
#include <SDL2/SDL.h>
#include "ship/Context.h"
#include "ship/controller/controldeck/ControlDeck.h"
#include "ship/window/Window.h"
#include "ship/window/gui/Gui.h"
#ifdef __VISIONOS__
#include "ship/controller/physicaldevice/SenseControllers.h"
#endif

namespace Ship {

SDLAddRemoveDeviceEventHandler::~SDLAddRemoveDeviceEventHandler() {
}

void SDLAddRemoveDeviceEventHandler::InitElement() {
}

void SDLAddRemoveDeviceEventHandler::DrawElement() {
}

void SDLAddRemoveDeviceEventHandler::UpdateElement() {
#ifdef __VISIONOS__
    // Attaching/detaching the merged Sense controller queues the add/remove events read below.
    SenseControllers::Update();
#endif
    SDL_PumpEvents();
    SDL_Event event;
    bool changed = false;
    auto manager = Context::GetRawInstance()->GetControlDeck()->GetConnectedPhysicalDeviceManager();
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_CONTROLLERDEVICEADDED, SDL_CONTROLLERDEVICEREMAPPED) > 0) {
        switch (event.type) {
            case SDL_CONTROLLERDEVICEADDED:
                // SDL supplies a device index only for the add event.
                manager->HandlePhysicalDeviceConnect(event.cdevice.which);
                changed = true;
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                manager->HandlePhysicalDeviceDisconnect(event.cdevice.which);
                changed = true;
                break;
            case SDL_CONTROLLERDEVICEREMAPPED:
                manager->HandlePhysicalDeviceRemap(event.cdevice.which);
                changed = true;
                break;
        }
    }

    changed = manager->ReconcileIfNeeded("active-check") || changed;

    // The connected controller set changed, so re-point the ImGui gamepad
    // backend at it (keeps menu navigation working across hotplug).
    if (changed) {
        auto window = Context::GetRawInstance()->GetWindow();
        if (window != nullptr && window->GetGui() != nullptr) {
            window->GetGui()->RefreshImGuiGamepads();
        }
    }
}
} // namespace Ship
