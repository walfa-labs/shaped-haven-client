#include "input/InputManager.h"

// The client is built on Windows, where the DLL search path starts at the
// executable's directory and SDL2.dll ships next to it.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>

namespace haven {

namespace {

// ---------------------------------------------------------------------------
// Minimal SDL2 GameController ABI.
//
// The Ogre SDK ships SDL2.dll — OgreBites links it for its window and input
// layer — but no SDL headers and no import library, so the few functions the
// gamepad layer needs are declared here and resolved from the DLL the process
// already has loaded. The signatures mirror SDL2's own headers; the ABI is
// stable across the 2.x line OgreBites is built against.
// ---------------------------------------------------------------------------

constexpr uint32_t kSdlInitGamecontroller = 0x00002000u;

constexpr int kSdlAxisMax = 32767;

// SDL_GameControllerButton, in SDL2's own order.
constexpr int kBtnA = 0;
constexpr int kBtnB = 1;
constexpr int kBtnX = 2;
constexpr int kBtnY = 3;
constexpr int kBtnBack = 4;
constexpr int kBtnGuide = 5;
constexpr int kBtnStart = 6;
constexpr int kBtnLeftStick = 7;
constexpr int kBtnRightStick = 8;
constexpr int kBtnLeftShoulder = 9;
constexpr int kBtnRightShoulder = 10;
constexpr int kBtnDpadUp = 11;
constexpr int kBtnDpadDown = 12;
constexpr int kBtnDpadLeft = 13;
constexpr int kBtnDpadRight = 14;

// SDL_GameControllerAxis, in SDL2's own order.
constexpr int kAxisLeftX = 0;
constexpr int kAxisLeftY = 1;

struct SDL_GameController;

using SdlInitSubSystem = int (*)(uint32_t flags);
using SdlQuitSubSystem = void (*)(uint32_t flags);
using SdlNumJoysticks = int (*)();
using SdlIsGameController = int (*)(int joystick_index);
using SdlGameControllerOpen = SDL_GameController* (*)(int joystick_index);
using SdlGameControllerClose = void (*)(SDL_GameController* gamecontroller);
using SdlGameControllerGetAttached = int (*)(SDL_GameController* gamecontroller);
using SdlGameControllerGetButton = uint8_t (*)(SDL_GameController* gamecontroller, int button);
using SdlGameControllerGetAxis = int16_t (*)(SDL_GameController* gamecontroller, int axis);
using SdlGameControllerName = const char* (*)(SDL_GameController* gamecontroller);
using SdlGameControllerUpdate = void (*)();

/// The resolved SDL2 function pointers, one instance per process. The
/// gamepad layer is permanently unavailable when the DLL is not mapped, which
/// leaves the keyboard path fully functional.
struct Sdl2 {
    SdlInitSubSystem InitSubSystem = nullptr;
    SdlQuitSubSystem QuitSubSystem = nullptr;
    SdlNumJoysticks NumJoysticks = nullptr;
    SdlIsGameController IsGameController = nullptr;
    SdlGameControllerOpen GameControllerOpen = nullptr;
    SdlGameControllerClose GameControllerClose = nullptr;
    SdlGameControllerGetAttached GameControllerGetAttached = nullptr;
    SdlGameControllerGetButton GameControllerGetButton = nullptr;
    SdlGameControllerGetAxis GameControllerGetAxis = nullptr;
    SdlGameControllerName GameControllerName = nullptr;
    SdlGameControllerUpdate GameControllerUpdate = nullptr;

    HMODULE module = nullptr;
    bool ok = false;

    bool load()
    {
        if (ok) return true;
        // OgreBites.dll has a static import on SDL2.dll, so the module is
        // already mapped into the process by the time main runs; LoadLibrary
        // with the bare module name returns that mapping.
        module = ::LoadLibraryA("SDL2.dll");
        if (module == nullptr) return false;

        auto resolve = [this](const char* name) { return ::GetProcAddress(module, name); };
        InitSubSystem = reinterpret_cast<SdlInitSubSystem>(resolve("SDL_InitSubSystem"));
        QuitSubSystem = reinterpret_cast<SdlQuitSubSystem>(resolve("SDL_QuitSubSystem"));
        NumJoysticks = reinterpret_cast<SdlNumJoysticks>(resolve("SDL_NumJoysticks"));
        IsGameController = reinterpret_cast<SdlIsGameController>(resolve("SDL_IsGameController"));
        GameControllerOpen = reinterpret_cast<SdlGameControllerOpen>(resolve("SDL_GameControllerOpen"));
        GameControllerClose = reinterpret_cast<SdlGameControllerClose>(resolve("SDL_GameControllerClose"));
        GameControllerGetAttached = reinterpret_cast<SdlGameControllerGetAttached>(resolve("SDL_GameControllerGetAttached"));
        GameControllerGetButton = reinterpret_cast<SdlGameControllerGetButton>(resolve("SDL_GameControllerGetButton"));
        GameControllerGetAxis = reinterpret_cast<SdlGameControllerGetAxis>(resolve("SDL_GameControllerGetAxis"));
        GameControllerName = reinterpret_cast<SdlGameControllerName>(resolve("SDL_GameControllerName"));
        GameControllerUpdate = reinterpret_cast<SdlGameControllerUpdate>(resolve("SDL_GameControllerUpdate"));

        ok = InitSubSystem != nullptr && QuitSubSystem != nullptr && NumJoysticks != nullptr &&
             IsGameController != nullptr && GameControllerOpen != nullptr && GameControllerClose != nullptr &&
             GameControllerGetAttached != nullptr && GameControllerGetButton != nullptr &&
             GameControllerGetAxis != nullptr && GameControllerName != nullptr && GameControllerUpdate != nullptr;
        if (!ok)
        {
            ::FreeLibrary(module);
            module = nullptr;
        }
        return ok;
    }
};

Sdl2& sdl()
{
    static Sdl2 instance;
    return instance;
}

/// The open controller as the SDL type: the header stores it as void* so no
/// SDL declaration leaks into it, and the pointer conversions are explicit.
SDL_GameController* pad(void* controller)
{
    return static_cast<SDL_GameController*>(controller);
}

/// Loads SDL2.dll once and brings up the gamecontroller subsystem. OgreBites
/// brings up the video/event subsystems for its own window; the gamecontroller
/// subsystem is refcounted, so this is safe whether or not it is already up.
/// Returns false exactly once's worth: a failed load is not retried, and the
/// gamepad is simply absent.
bool ensure_sdl_ready()
{
    Sdl2& lib = sdl();
    static bool attempted = false;
    if (attempted) return lib.ok;
    attempted = true;
    if (!lib.load()) return false;
    if (lib.InitSubSystem(kSdlInitGamecontroller) != 0)
    {
        lib.ok = false;
        return false;
    }
    return true;
}

/// Normalised stick deflection: raw SDL axis to -1..1 past the deadzone,
/// rescaled so the edge of the deadzone is zero and full deflection is one.
/// SDL's Y axis runs up-negative and this game's +y is up, so `negate` flips Y.
constexpr float kStickDeadzone = 0.18f;

float axis_to_unit(int16_t raw, bool negate)
{
    const float v = static_cast<float>(raw) / 32768.0f * (negate ? -1.0f : 1.0f);
    if (v > kStickDeadzone) return (v - kStickDeadzone) / (1.0f - kStickDeadzone);
    if (v < -kStickDeadzone) return (v + kStickDeadzone) / (1.0f - kStickDeadzone);
    return 0.0f;
}

}  // namespace

InputManager::InputManager()
{
    ensure_sdl_ready();
}

InputManager::~InputManager()
{
    shutdown();
}

void InputManager::shutdown()
{
    Sdl2& lib = sdl();
    if (pad(controller_) != nullptr && lib.ok)
    {
        lib.GameControllerClose(pad(controller_));
        controller_ = nullptr;
    }
    controller_name_[0] = '\0';
    buttons_ = 0;
    prev_buttons_ = 0;
    dash_edge_ = false;
    added_ = false;
    removed_ = false;
    if (lib.ok) lib.QuitSubSystem(kSdlInitGamecontroller);
}

bool InputManager::keyPressed(int key_code)
{
    input_.key_down(key_code);
    return false;
}

bool InputManager::keyReleased(int key_code)
{
    input_.key_up(key_code);
    return false;
}

void InputManager::refresh_controller()
{
    if (!ensure_sdl_ready()) return;

    Sdl2& lib = sdl();
    if (pad(controller_) != nullptr)
    {
        // A controller the OS unplugged reports not-attached; close it so the
        // next scan can open whatever is plugged in then.
        if (lib.GameControllerGetAttached(pad(controller_)) == 0)
        {
            lib.GameControllerClose(pad(controller_));
            controller_ = nullptr;
            removed_ = true;
            controller_name_[0] = '\0';
        }
        return;
    }

    // The first gamepad SDL reports is the one this client opens.
    const int count = lib.NumJoysticks();
    for (int i = 0; i < count; ++i)
    {
        if (lib.IsGameController(i) == 0) continue;
        controller_ = lib.GameControllerOpen(i);
        if (pad(controller_) != nullptr)
        {
            const char* name = lib.GameControllerName(pad(controller_));
            std::snprintf(controller_name_, sizeof(controller_name_), "%s",
                          name != nullptr ? name : "gamepad");
            added_ = true;
        }
        break;
    }
}

void InputManager::update(float /*dt*/)
{
    if (!ensure_sdl_ready()) return;

    Sdl2& lib = sdl();
    lib.GameControllerUpdate();
    refresh_controller();

    // The keyboard half of dash, edge-detected here once per rendered frame
    // rather than on read: a frame where no fixed simulation step runs must
    // not leave the edge unobserved, and a reader that runs several times in
    // one frame must see it exactly once. dash_pressed() only consumes.
    const bool shift = input_.held(kKeyShiftLeft) || input_.held(kKeyShiftRight);
    if (shift && !prev_shift_) dash_edge_ = true;
    prev_shift_ = shift;

    prev_buttons_ = buttons_;
    uint16_t now = 0;
    if (pad(controller_) != nullptr)
    {
        const auto pressed = [&lib, this](int button) {
            return lib.GameControllerGetButton(pad(controller_), button) != 0;
        };
        if (pressed(kBtnA)) now |= 1u << kBtnA;
        if (pressed(kBtnB)) now |= 1u << kBtnB;
        if (pressed(kBtnX)) now |= 1u << kBtnX;
        if (pressed(kBtnY)) now |= 1u << kBtnY;
        if (pressed(kBtnBack)) now |= 1u << kBtnBack;
        if (pressed(kBtnGuide)) now |= 1u << kBtnGuide;
        if (pressed(kBtnStart)) now |= 1u << kBtnStart;
        if (pressed(kBtnLeftStick)) now |= 1u << kBtnLeftStick;
        if (pressed(kBtnRightStick)) now |= 1u << kBtnRightStick;
        if (pressed(kBtnLeftShoulder)) now |= 1u << kBtnLeftShoulder;
        if (pressed(kBtnRightShoulder)) now |= 1u << kBtnRightShoulder;
        if (pressed(kBtnDpadUp)) now |= 1u << kBtnDpadUp;
        if (pressed(kBtnDpadDown)) now |= 1u << kBtnDpadDown;
        if (pressed(kBtnDpadLeft)) now |= 1u << kBtnDpadLeft;
        if (pressed(kBtnDpadRight)) now |= 1u << kBtnDpadRight;

        // A press is a button that is down now and was not down last frame,
        // so holding a button repeats nothing.
        const uint16_t dash_mask = (1u << kBtnLeftShoulder) | (1u << kBtnRightShoulder) | (1u << kBtnB);
        if ((now & dash_mask) != 0 && (prev_buttons_ & dash_mask) == 0) dash_edge_ = true;
        if ((now & (1u << kBtnStart)) != 0 && (prev_buttons_ & (1u << kBtnStart)) == 0)
            input_.request_restart();
    }
    buttons_ = now;
}

Vec2 InputManager::move_vector()
{
    Vec2 move = input_.move_vector();

    if (!sdl().ok || pad(controller_) == nullptr) return move;

    Sdl2& lib = sdl();
    const float nx = axis_to_unit(lib.GameControllerGetAxis(pad(controller_), kAxisLeftX), false);
    const float ny = axis_to_unit(lib.GameControllerGetAxis(pad(controller_), kAxisLeftY), true);
    if (nx == 0.0f && ny == 0.0f) return move;

    // Keyboard and stick are additive, then clamped to one unit so a combined
    // diagonal is not faster than a straight stick push.
    Vec2 combined = move + Vec2{nx, ny};
    const float len = length(combined);
    return len > 1.0f ? combined * (1.0f / len) : combined;
}

bool InputManager::dash_pressed()
{
    const bool pressed = dash_edge_;
    dash_edge_ = false;
    return pressed;
}

bool InputManager::ui_up() const
{
    if (pad(controller_) != nullptr && (buttons_ & (1u << kBtnDpadUp)) != 0) return true;
    return input_.held(InputState::kKeyUp);
}

bool InputManager::ui_down() const
{
    if (pad(controller_) != nullptr && (buttons_ & (1u << kBtnDpadDown)) != 0) return true;
    return input_.held(InputState::kKeyDown);
}

bool InputManager::ui_left() const
{
    if (pad(controller_) != nullptr && (buttons_ & (1u << kBtnDpadLeft)) != 0) return true;
    return input_.held(InputState::kKeyLeft);
}

bool InputManager::ui_right() const
{
    if (pad(controller_) != nullptr && (buttons_ & (1u << kBtnDpadRight)) != 0) return true;
    return input_.held(InputState::kKeyRight);
}

bool InputManager::ui_confirm() const
{
    if (pad(controller_) != nullptr && (buttons_ & (1u << kBtnA)) != 0) return true;
    return input_.held(InputState::kKeyEnter);
}

bool InputManager::ui_cancel() const
{
    if (pad(controller_) != nullptr && (buttons_ & (1u << kBtnB)) != 0) return true;
    return input_.held(InputState::kKeyEscape);
}

bool InputManager::restart_pressed()
{
    const bool pressed = input_.restart_requested();
    input_.consume_restart();
    return pressed;
}

bool InputManager::controller_just_added()
{
    const bool added = added_;
    added_ = false;
    return added;
}

bool InputManager::controller_just_removed()
{
    const bool removed = removed_;
    removed_ = false;
    return removed;
}

}  // namespace haven
