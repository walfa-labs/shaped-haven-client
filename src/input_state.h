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

private:
    std::unordered_map<int, bool> held_;
    bool quit_ = false;
    int last_key_ = 0;
    const std::vector<Vec2>* script_ = nullptr;
    mutable size_t script_index_ = 0;
};

/// Expands an input script into one move vector per tick. Each non-comment line
/// is `<move_x> <move_y> <ticks>`; the final entry repeats for the rest of the
/// run. Returns false if the file cannot be read or parsed.
bool load_input_script(const std::string& path, std::vector<Vec2>& out);

}  // namespace haven
