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

    Vec2 move_vector() const;
    bool quit_requested() const { return quit_; }
    void consume_quit() { quit_ = false; }

    /// Last keycode seen, for the verbose key log.
    int last_key() const { return last_key_; }

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

    /// The choice script: one line per run event, in chronological order — a
    /// choice index 0..2 for each level-up offer, the literal `restart` for
    /// each game over. Consumed one line per event, so a fully unattended run
    /// is `--input-script` for movement plus `--choice-script` for picks.
    void set_choice_script(const std::vector<int8_t>& picks);
    bool has_choice_script() const { return choice_script_ != nullptr; }

    /// Consumes the next scripted line: 0..2 is a choice index, -1 a restart
    /// marker, -3 when the script is exhausted.
    int8_t consume_scripted_line();

private:
    std::unordered_map<int, bool> held_;
    bool quit_ = false;
    int last_key_ = 0;
    int pending_choice_ = -1;
    bool restart_ = false;
    const std::vector<Vec2>* script_ = nullptr;
    mutable size_t script_index_ = 0;
    const std::vector<int8_t>* choice_script_ = nullptr;
    size_t choice_index_ = 0;
};

/// Expands an input script into one move vector per tick. Each non-comment line
/// is `<move_x> <move_y> <ticks>`; the final entry repeats for the rest of the
/// run. Returns false if the file cannot be read or parsed.
bool load_input_script(const std::string& path, std::vector<Vec2>& out);

/// Loads a choice script: each non-comment line is a choice index 0..2 or the
/// literal `restart`, one line per run event in chronological order. Returns
/// false if the file cannot be read or parsed.
bool load_choice_script(const std::string& path, std::vector<int8_t>& out);

}  // namespace haven
