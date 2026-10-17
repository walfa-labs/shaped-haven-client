#pragma once

#include "game_math.h"
#include "input_state.h"

#include <cstdint>
#include <string>
#include <vector>

namespace haven {

/// The unified input path: keyboard through the wrapped InputState plus an
/// SDL2 gamepad read directly from the SDL2.dll OgreBites already loads.
///
/// The Ogre SDK ships SDL2.dll without the SDL headers, so the gamepad layer
/// declares the handful of SDL_GameController functions it needs and resolves
/// them from the already-loaded DLL at startup; see InputManager.cpp. When the
/// DLL is not found the manager still works, keyboard-only.
///
/// Keyboard events arrive through keyPressed/keyReleased, which the renderer's
/// own OgreBites::InputListener hook feeds from the window; the manager then
/// forwards them into the InputState, so every keyboard path the scripts and
/// the text field use is unchanged. update() polls the gamepad once per
/// rendered frame and edge-detects its buttons; main() calls it next to
/// advance_key_script().
///
/// The manager is deliberately free of any Ogre dependency — the input layer
/// is unit-tested without a renderer, the same reason InputState is. The gamepad
/// is additive: with no controller attached every accessor answers exactly what
/// the keyboard alone would, so a run with no gamepad is byte-identical to one
/// driven by InputState on its own.
class InputManager {
public:
    // SDLK_LSHIFT / SDLK_RSHIFT, as OgreBites' SDL layer delivers them.
    // Restated here for the same reason InputState restates the arrow keys:
    // the SDK ships SDL2.dll without the SDL headers.
    static constexpr int kKeyShiftLeft = (1 << 30) | 0xE1;
    static constexpr int kKeyShiftRight = (1 << 30) | 0xE2;

    InputManager();
    ~InputManager();

    InputManager(const InputManager&) = delete;
    InputManager& operator=(const InputManager&) = delete;

    // Keyboard events from the renderer's OgreBites listener, which passes
    // the SDL keycode through. Both return false so any further listener in
    // the chain still sees the event.
    bool keyPressed(int key_code);
    bool keyReleased(int key_code);

    /// Polls the gamepad and re-evaluates its edge detection. Call once per
    /// rendered frame before any input is read.
    void update(float dt);

    /// WASD (or the move script) plus the left stick, combined so the result
    /// is never longer than one unit. Returns the keyboard vector untouched
    /// when no stick is deflected, so keyboard-only runs are unchanged.
    Vec2 move_vector();

    /// True once per press of Shift or a shoulder/B button. Consumed on read.
    bool dash_pressed();

    /// Menu navigation: arrow keys or the D-pad, held.
    bool ui_up() const;
    bool ui_down() const;
    bool ui_left() const;
    bool ui_right() const;

    /// Enter or the A button, held.
    bool ui_confirm() const;

    /// Escape or the B button, held.
    bool ui_cancel() const;

    /// True once per press of R or the Start button, consumed on read. A
    /// gamepad press is raised into the keyboard state by update(), so both
    /// sources answer through this one flag; the game-over restart path in
    /// GameSession reads it through the restart gate instead.
    bool restart_pressed();

    /// The wrapped keyboard state: the input scripts, the level-up choice
    /// keys, the name field's text capture and the verbose key log all live
    /// there, and GameSession holds a reference to it.
    InputState& keyboard() { return input_; }
    const InputState& keyboard() const { return input_; }

    /// The keyboard surface main() drives the menus and the name field with.
    /// Forwarded rather than reimplemented so InputState stays the one place
    /// the scripted-input machinery exists.
    bool held(int key_code) const { return input_.held(key_code); }
    void set_escape_quits(bool enabled) { input_.set_escape_quits(enabled); }
    void set_text_capture(bool enabled) { input_.set_text_capture(enabled); }
    std::string take_typed_text() { return input_.take_typed_text(); }
    int last_key() const { return input_.last_key(); }
    void set_script(const std::vector<Vec2>& script) { input_.set_script(script); }
    void set_choice_script(const std::vector<int8_t>& picks) { input_.set_choice_script(picks); }
    void set_key_script(const std::vector<int>& keys) { input_.set_key_script(keys); }
    bool key_script_finished() const { return input_.key_script_finished(); }
    void advance_key_script() { input_.advance_key_script(); }
    bool has_choice_script() const { return input_.has_choice_script(); }
    int8_t peek_scripted_line() const { return input_.peek_scripted_line(); }
    int8_t take_scripted_choice() { return input_.take_scripted_choice(); }
    bool take_scripted_restart() { return input_.take_scripted_restart(); }

    bool quit_requested() const { return input_.quit_requested(); }
    void consume_quit() { input_.consume_quit(); }

    /// The level-up choice keys (1/2/3) and the restart key are keyboard
    /// only: a gamepad cannot type, and the modal is answered with the same
    /// pending_choice() the scripts and GameSession read.
    int pending_choice() const { return input_.pending_choice(); }
    void consume_choice() { input_.consume_choice(); }

    // Gamepad status, for the HUD and the verbose log.
    bool controller_attached() const { return controller_ != nullptr; }
    const char* controller_name() const { return controller_name_; }

    /// True once after a controller was connected since the last call.
    bool controller_just_added();
    /// True once after the open controller was disconnected since the last
    /// call.
    bool controller_just_removed();

    /// Releases the controller and the resolved SDL functions. Called from the
    /// destructor; the gamepad is process-wide state, so this only runs when
    /// the client tears down.
    void shutdown();

private:
    /// Scans SDL_NumJoysticks for the first gamepad, opens it, and answers
    /// whether the open controller is still present. Fills added/removed
    /// flags when the topology changes. The SDL2 function pointers are resolved
    /// once per process by ensure_sdl_ready() in the .cpp.
    void refresh_controller();

    /// Current gamepad button states and last frame's, so a press is a button
    /// that is down now and was not down a frame ago.
    uint16_t buttons_ = 0;
    uint16_t prev_buttons_ = 0;

    /// Set by update() from a fresh press, cleared when dash_pressed()
    /// consumes it.
    bool dash_edge_ = false;

    /// Last frame's Shift state, for the keyboard half of dash_pressed()'s
    /// edge detection.
    bool prev_shift_ = false;

    /// Set by refresh_controller() when the connected controller changed,
    /// cleared by the *_just_* accessors.
    bool added_ = false;
    bool removed_ = false;

    /// The one gamepad this client opens: the first one SDL reports.
    void* controller_ = nullptr;
    char controller_name_[64] = {};

    InputState input_;
};

}  // namespace haven
