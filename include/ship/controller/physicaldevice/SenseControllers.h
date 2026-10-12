#pragma once

#ifdef __VISIONOS__
namespace Ship::SenseControllers {

// visionOS exposes each PlayStation VR2 Sense controller as its own GCController, which SDL's
// MFi driver turns into a separate joystick whose stick is not mapped ("Thumbstick" is not a
// standard element name). While at least one is connected, a virtual SDL game controller
// ("PS VR2 Sense Pair") stands in for both, read straight from GameController:
//   left hand:  stick = left stick, L3, Grip = L1, Trigger = L2, Square = X, Triangle = Y, Create = BACK
//   right hand: stick = right stick, R3, Grip = R1, Trigger = R2, Cross = A, Circle = B, Options = START
// With only one hand connected its stick (and stick click) drives the LEFT stick and its face
// buttons become A (lower) and B (upper); Create stays BACK and Options stays START.

// Attaches or detaches the virtual controller as Sense controllers come and go. Game thread,
// once per frame, before SDL events are pumped.
void Update();

// True for a raw single-hand Sense joystick (the virtual controller replaces it). The ControlDeck
// and the ImGui gamepad binding skip these so input is not doubled.
bool IsReplacedDevice(int sdlDeviceIndex);

} // namespace Ship::SenseControllers
#endif
