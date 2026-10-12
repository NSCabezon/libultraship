#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <SDL2/SDL.h>

#include "ship/controller/physicaldevice/ControllerSlotAssignments.h"

namespace Ship {

/** Owns connected SDL2 gamepad handles and their N64 player slots. */
class ConnectedPhysicalDeviceManager {
  public:
    ConnectedPhysicalDeviceManager();
    ~ConnectedPhysicalDeviceManager();

    std::unordered_map<int32_t, SDL_GameController*> GetConnectedSDLGamepadsForPort(uint8_t portIndex);
    std::unordered_map<int32_t, std::string> GetConnectedSDLGamepadNames();
    std::unordered_set<int32_t> GetIgnoredInstanceIdsForPort(uint8_t portIndex);
    bool PortIsIgnoringInstanceId(uint8_t portIndex, int32_t instanceId);
    void IgnoreInstanceIdForPort(uint8_t portIndex, int32_t instanceId);
    void UnignoreInstanceIdForPort(uint8_t portIndex, int32_t instanceId);

    void HandlePhysicalDeviceConnect(int32_t sdlDeviceIndex);
    void HandlePhysicalDeviceDisconnect(int32_t sdlJoystickInstanceId);
    void HandlePhysicalDeviceRemap(int32_t sdlJoystickInstanceId);
    // Both return true only when a controller handle was opened or closed.
    bool RefreshConnectedSDLGamepads(const char* reason);
    bool ReconcileIfNeeded(const char* reason);

  private:
    struct SDLGamepadRecord {
        SDL_GameController* gamepad = nullptr;
        std::string name;
        std::string stableId;
    };

    std::string BuildStableId(SDL_GameController* gamepad, const char* guid, const std::string& name) const;
    void CloseGamepad(int32_t instanceId, const SDLGamepadRecord& record, const char* reason);

    std::unordered_map<int32_t, SDLGamepadRecord> mConnectedSDLGamepads;
    ControllerSlotAssignments mSlotAssignments;
    Uint64 mLastReconcileTicks = 0;
    bool mNeedsReconcile = true;
};

} // namespace Ship
