#include "ship/controller/physicaldevice/ConnectedPhysicalDeviceManager.h"
#ifdef __VISIONOS__
#include "ship/controller/physicaldevice/SenseControllers.h"
#endif

#include <utility>
#include <vector>
#include <string_view>
#include <spdlog/spdlog.h>

namespace Ship {
namespace {
constexpr Uint64 ActiveReconcileIntervalMs = 1000;
}

ConnectedPhysicalDeviceManager::ConnectedPhysicalDeviceManager() {
}

ConnectedPhysicalDeviceManager::~ConnectedPhysicalDeviceManager() {
    for (const auto& [instanceId, record] : mConnectedSDLGamepads) {
        CloseGamepad(instanceId, record, "shutdown");
    }
}

std::unordered_map<int32_t, SDL_GameController*>
ConnectedPhysicalDeviceManager::GetConnectedSDLGamepadsForPort(uint8_t portIndex) {
    std::unordered_map<int32_t, SDL_GameController*> result;
    auto assignedInstanceId = mSlotAssignments.GetInstanceId(portIndex);
    if (!assignedInstanceId.has_value()) {
        return result;
    }

    auto record = mConnectedSDLGamepads.find(*assignedInstanceId);
    if (record == mConnectedSDLGamepads.end() || record->second.gamepad == nullptr ||
        !SDL_GameControllerGetAttached(record->second.gamepad)) {
        // Stop gameplay reads immediately. The next manager update will close
        // and replace this handle even if SDL's removal event was missed.
        mNeedsReconcile = true;
        return result;
    }

    result[*assignedInstanceId] = record->second.gamepad;
    return result;
}

std::unordered_map<int32_t, std::string> ConnectedPhysicalDeviceManager::GetConnectedSDLGamepadNames() {
    std::unordered_map<int32_t, std::string> names;
    for (const auto& [instanceId, record] : mConnectedSDLGamepads) {
        if (record.gamepad != nullptr && SDL_GameControllerGetAttached(record.gamepad)) {
            names[instanceId] = record.name;
        }
    }
    return names;
}

std::unordered_set<int32_t> ConnectedPhysicalDeviceManager::GetIgnoredInstanceIdsForPort(uint8_t portIndex) {
    std::unordered_set<int32_t> ignored;
    for (const auto& [instanceId, record] : mConnectedSDLGamepads) {
        if (mSlotAssignments.GetSlot(instanceId) != portIndex) {
            ignored.insert(instanceId);
        }
    }
    return ignored;
}

bool ConnectedPhysicalDeviceManager::PortIsIgnoringInstanceId(uint8_t portIndex, int32_t instanceId) {
    return mSlotAssignments.GetSlot(instanceId) != portIndex;
}

void ConnectedPhysicalDeviceManager::IgnoreInstanceIdForPort(uint8_t portIndex, int32_t instanceId) {
    if (mSlotAssignments.Unassign(instanceId, portIndex)) {
        SPDLOG_INFO("Controller instance {} released from player {} by settings", instanceId, portIndex + 1);
    }
}

void ConnectedPhysicalDeviceManager::UnignoreInstanceIdForPort(uint8_t portIndex, int32_t instanceId) {
    if (mSlotAssignments.Assign(instanceId, portIndex)) {
        SPDLOG_INFO("Controller instance {} assigned to player {} by settings", instanceId, portIndex + 1);
    }
}

void ConnectedPhysicalDeviceManager::HandlePhysicalDeviceConnect(int32_t sdlDeviceIndex) {
    SPDLOG_INFO("Controller add event for SDL device index {}", sdlDeviceIndex);
    RefreshConnectedSDLGamepads("add-event");
}

void ConnectedPhysicalDeviceManager::HandlePhysicalDeviceDisconnect(int32_t sdlJoystickInstanceId) {
    SPDLOG_INFO("Controller remove event for instance {}", sdlJoystickInstanceId);
    RefreshConnectedSDLGamepads("remove-event");
}

void ConnectedPhysicalDeviceManager::HandlePhysicalDeviceRemap(int32_t sdlJoystickInstanceId) {
    SPDLOG_INFO("Controller remap event for instance {}", sdlJoystickInstanceId);
    RefreshConnectedSDLGamepads("remap-event");
}

std::string ConnectedPhysicalDeviceManager::BuildStableId(SDL_GameController* gamepad, const char* guid,
                                                          const std::string& name) const {
    const char* serial = SDL_GameControllerGetSerial(gamepad);
    return std::string(guid) + "|" + (serial != nullptr && serial[0] != '\0' ? serial : name);
}

void ConnectedPhysicalDeviceManager::CloseGamepad(int32_t instanceId, const SDLGamepadRecord& record,
                                                  const char* reason) {
    auto slot = mSlotAssignments.GetSlot(instanceId);
    if (slot.has_value()) {
        SPDLOG_INFO("Controller instance {} released from player {} during {}", instanceId, *slot + 1, reason);
    }
    if (record.gamepad != nullptr) {
        SPDLOG_INFO("Closing controller instance {} ({}) during {}", instanceId, record.name, reason);
        SDL_GameControllerClose(record.gamepad);
    }
}

bool ConnectedPhysicalDeviceManager::RefreshConnectedSDLGamepads(const char* reason) {
    if (std::string_view(reason) != "active-check") {
        SPDLOG_INFO("Reconciling controllers: {}", reason);
    }

    std::unordered_map<int32_t, SDLGamepadRecord> nextGamepads;
    std::vector<ControllerSlotAssignments::Device> discoveredDevices;
    static SDL_JoystickGUID sZeroGuid;
    bool changed = false;

    for (int32_t deviceIndex = 0; deviceIndex < SDL_NumJoysticks(); ++deviceIndex) {
        SDL_JoystickGUID deviceGuid = SDL_JoystickGetDeviceGUID(deviceIndex);
        if (SDL_memcmp(&deviceGuid, &sZeroGuid, sizeof(deviceGuid)) == 0) {
            SPDLOG_WARN("Controller device index {} returned a zero GUID during {}", deviceIndex, reason);
            continue;
        }

        char guid[33] = "";
        SDL_JoystickGetGUIDString(deviceGuid, guid, sizeof(guid));
        if (!SDL_IsGameController(deviceIndex)) {
            SPDLOG_WARN("SDL joystick device index {} (GUID {}) is not a mapped game controller", deviceIndex, guid);
            continue;
        }
#ifdef __VISIONOS__
        if (SenseControllers::IsReplacedDevice(deviceIndex)) {
            continue; // read through the merged virtual controller instead
        }
#endif

        int32_t instanceId = SDL_JoystickGetDeviceInstanceID(deviceIndex);
        if (instanceId < 0) {
            SPDLOG_ERROR("Could not resolve controller device index {} during {}: {}", deviceIndex, reason,
                         SDL_GetError());
            continue;
        }

        auto existing = mConnectedSDLGamepads.find(instanceId);
        if (existing != mConnectedSDLGamepads.end() && existing->second.gamepad != nullptr &&
            SDL_GameControllerGetAttached(existing->second.gamepad)) {
            nextGamepads.emplace(instanceId, existing->second);
            discoveredDevices.push_back({ instanceId, existing->second.stableId });
            continue;
        }

        SDL_GameController* gamepad = SDL_GameControllerOpen(deviceIndex);
        if (gamepad == nullptr) {
            SPDLOG_ERROR("Could not open controller device index {} (GUID {}) during {}: {}", deviceIndex, guid,
                         reason, SDL_GetError());
            continue;
        }

        int32_t openedInstanceId = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gamepad));
        if (openedInstanceId < 0) {
            SPDLOG_ERROR("Could not resolve opened controller device index {} during {}: {}", deviceIndex, reason,
                         SDL_GetError());
            SDL_GameControllerClose(gamepad);
            continue;
        }

        const char* controllerName = SDL_GameControllerName(gamepad);
        std::string name = controllerName != nullptr ? controllerName : guid;
        SDLGamepadRecord record = { gamepad, name, BuildStableId(gamepad, guid, name) };
        auto [inserted, wasInserted] = nextGamepads.emplace(openedInstanceId, std::move(record));
        if (!wasInserted) {
            SPDLOG_WARN("Ignoring duplicate controller instance {} at device index {}", openedInstanceId,
                        deviceIndex);
            SDL_GameControllerClose(gamepad);
            continue;
        }
        discoveredDevices.push_back({ openedInstanceId, inserted->second.stableId });
        changed = true;
        SPDLOG_INFO("Opened controller device index {} as instance {} ({}) during {}", deviceIndex, openedInstanceId,
                    name, reason);
    }

    for (const auto& [instanceId, record] : mConnectedSDLGamepads) {
        auto replacement = nextGamepads.find(instanceId);
        if (replacement == nextGamepads.end() || replacement->second.gamepad != record.gamepad) {
            CloseGamepad(instanceId, record, reason);
            changed = true;
        }
    }

    std::unordered_map<int32_t, std::optional<uint8_t>> previousSlots;
    for (const auto& [instanceId, record] : mConnectedSDLGamepads) {
        previousSlots[instanceId] = mSlotAssignments.GetSlot(instanceId);
    }
    mSlotAssignments.Reconcile(discoveredDevices);
    for (const auto& device : discoveredDevices) {
        auto slot = mSlotAssignments.GetSlot(device.instanceId);
        auto previous = previousSlots.find(device.instanceId);
        if (slot.has_value() &&
            (previous == previousSlots.end() || previous->second != slot)) {
            SPDLOG_INFO("Controller instance {} assigned to player {} during {}", device.instanceId, *slot + 1,
                        reason);
        } else if (!slot.has_value()) {
            SPDLOG_WARN("Controller instance {} has no free player slot during {}", device.instanceId, reason);
        }
    }

    mConnectedSDLGamepads = std::move(nextGamepads);
    mLastReconcileTicks = SDL_GetTicks64();
    mNeedsReconcile = false;
    return changed;
}

bool ConnectedPhysicalDeviceManager::ReconcileIfNeeded(const char* reason) {
    Uint64 now = SDL_GetTicks64();
    if (!mNeedsReconcile && now - mLastReconcileTicks < ActiveReconcileIntervalMs) {
        return false;
    }
    // The periodic check usually finds the same handles; report only real changes so
    // listeners (the ImGui gamepad binding) do not re-enumerate every second.
    return RefreshConnectedSDLGamepads(reason);
}

} // namespace Ship
