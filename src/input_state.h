#pragma once

#include "game_math.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace haven {

/// Turns held keys into a normalised movement vector and tracks the quit key.
///
/// WASD and the arrow keys are both accepted; the vector is normalised so a
/// diagonal move is not faster than a straight one.
///
/// This is deliberately free of any Ogre dependency so the key mapping and the
/// movement maths can be unit tested on their own. The renderer forwards
/// OgreBites key events into key_down()/key_up().
class InputState {
public:
    static constexpr int kKeyW = 'w';
    static constexpr int kKeyA = 'a';
    static constexpr int kKeyS = 's';
    static constexpr int kKeyD = 'd';
    static constexpr int kKeyEscape = 27;

    // Level-up card selection and the game-over restart key.
    static constexpr int kKey1 = '1';
    static constexpr int kKey2 = '2';
    static constexpr int kKey3 = '3';
    static constexpr int kKeyR = 'r';

    /// The menu's confirm key. SDL2 delivers the return key as SDLK_RETURN.
    static constexpr int kKeyEnter = 13;

    /// SDLK_BACKSPACE, which the character select's name field deletes with.
    static constexpr int kKeyBackspace = 8;

    // OgreBites hands us SDL2 keycodes, but the SDK ships SDL2.dll without the
    // SDL headers, so the arrow keys are restated here. SDL2 defines
    // SDLK_*ARROW as SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_*ARROW) with
    // SDLK_SCANCODE_MASK == (1 << 30), and the arrow scancodes are
    // RIGHT = 79, LEFT = 80, DOWN = 81, UP = 82.
    static constexpr int kSdlScancodeMask = 1 << 30;
    static constexpr int kKeyLeft = kSdlScancodeMask | 80;
    static constexpr int kKeyRight = kSdlScancodeMask | 79;
    static constexpr int kKeyUp = kSdlScancodeMask | 82;
    static constexpr int kKeyDown = kSdlScancodeMask | 81;

    void key_down(int key_code);
    void key_up(int key_code);

    /// True while `key_code` is down. The main menu edge-detects on this: a
    /// press is a key that is held now and was not held last frame.
    bool held(int key_code) const;

    Vec2 move_vector() const;
    bool quit_requested() const { return quit_; }
    void consume_quit() { quit_ = false; }

    /// Last keycode seen, for the verbose key log.
    int last_key() const { return last_key_; }

    /// Escape quits the client. The character select screen uses escape as its
    /// back key instead, so it turns this off while it is up.
    void set_escape_quits(bool enabled) { escape_quits_ = enabled; }

    /// Turns collection of printable keycodes on for take_typed_text(). The
    /// character select's name field is the one caller; it is off by default so
    /// a run's movement keys do not accumulate in the buffer.
    void set_text_capture(bool enabled) { capture_text_ = enabled; }

    /// The printable characters typed since the last call, and clears them.
    /// main() drains this once per frame while the name field is on screen.
    std::string take_typed_text();

    /// When set, move_vector() returns scripted moves instead of reading the
    /// keyboard, one per call. Used by automated verification runs so the input
    /// path can be exercised without a focused window; see --input-script.
    void set_script(const std::vector<Vec2>& script);

    /// A level-up choice key pressed since the last consume: 0, 1 or 2, or -1
    /// when none is pending. The keys are 1/2/3, matching the card order.
    int pending_choice() const { return pending_choice_; }
    void consume_choice() { pending_choice_ = -1; }

    /// True when the restart key has been pressed since the last consume.
    bool restart_requested() const { return restart_; }
    void consume_restart() { restart_ = false; }

    /// Raises the restart request without a keyboard event: the gamepad's
    /// Start button sets this from InputManager::update(), so the pad and the
    /// keyboard feed the same one-shot path the restart gate guards.
    void request_restart() { restart_ = true; }

    /// The choice script: one line per run event, in chronological order — a
    /// choice index 0..2 for each level-up offer, the literal `restart` for
    /// each game over. Each line names the event it answers, so it is consumed
    /// only by that event: a marker is never spent on a different one, which is
    /// what would let an extra level-up eat the restart line and leave an
    /// unattended run stuck at the scoreboard. A fully unattended run is
    /// `--input-script` for movement plus `--choice-script` for picks.
    void set_choice_script(const std::vector<int8_t>& picks);
    bool has_choice_script() const { return choice_script_ != nullptr; }

    /// The next scripted line without consuming it: 0..2 is a choice index, -1
    /// a restart marker, -3 when the script is absent or exhausted.
    int8_t peek_scripted_line() const;

    /// Consumes the next line only when it is a choice index; when it is not,
    /// returns the line's own value (-1 restart marker, -3 exhausted) and
    /// leaves it in place.
    int8_t take_scripted_choice();

    /// Consumes the next line only when it is a restart marker; returns false
    /// and leaves the line in place when it names a level-up or the script is
    /// exhausted.
    bool take_scripted_restart();

    /// A script of key presses: the same idea as the move script, for the
    /// screens a run has not reached yet. The main menu and the character select
    /// read held() and the typed-text buffer, and this feeds both, so a menu
    /// flow can be driven without a focused window. See --key-script.
    void set_key_script(const std::vector<int>& keys);

    /// True once a script that was armed has run out of entries.
    bool key_script_finished() const;

    /// Advances the script by one frame: each entry is pressed on one frame and
    /// released on the next, so two presses of the same key in a row are two
    /// presses as the menu's edge detection sees them. Called once per rendered
    /// frame while a script is armed.
    void advance_key_script();

private:
    std::unordered_map<int, bool> held_;
    bool quit_ = false;
    bool escape_quits_ = true;
    bool capture_text_ = false;
    std::string typed_text_;
    int last_key_ = 0;
    int pending_choice_ = -1;
    bool restart_ = false;
    const std::vector<Vec2>* script_ = nullptr;
    mutable size_t script_index_ = 0;
    const std::vector<int8_t>* choice_script_ = nullptr;
    size_t choice_index_ = 0;
    const std::vector<int>* key_script_ = nullptr;
    size_t key_script_index_ = 0;
    bool key_script_releasing_ = false;
};

/// Expands an input script into one move vector per tick. Each non-comment line
/// is `<move_x> <move_y> <ticks>`; the final entry repeats for the rest of the
/// run. Returns false if the file cannot be read or parsed.
bool load_input_script(const std::string& path, std::vector<Vec2>& out);

/// Loads a choice script: each non-comment line is a choice index 0..2 or the
/// literal `restart`, one line per run event in chronological order. Returns
/// false if the file cannot be read or parsed.
bool load_choice_script(const std::string& path, std::vector<int8_t>& out);

/// Loads a key script: one key press per non-comment line, each held for one
/// rendered frame and released on the next. A line is a key name — `up`,
/// `down`, `left`, `right`, `enter`, `escape`, `backspace`, `space` — or a
/// single printable character, which is what the name field is typed with.
/// Returns false if the file cannot be read or parsed.
bool load_key_script(const std::string& path, std::vector<int>& out);

}  // namespace haven
