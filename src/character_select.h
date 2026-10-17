#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace haven {

/// The character select screen's state: the name being typed, the chosen race
/// and starting weapon, and which of the four fields the highlight is on.
///
/// The key rules live here rather than in the render loop — the field order,
/// the wraparound on Left/Right, the 16 byte name limit — so they can be tested
/// without a window. main.cpp turns key events into these calls and mirrors the
/// result into the HUD; the ids it hands `GameSession::connect` are the two
/// `C2S_HELLO` fields the server reads.
class CharacterSelectModel {
public:
    /// The four fields the Up/Down keys move between, in screen order.
    enum Field : int {
        kFieldName = 0,
        kFieldRace = 1,
        kFieldWeapon = 2,
        kFieldStart = 3,
        kFieldCount = 4,
    };

    /// The wire's name field: `C2S_HELLO` carries exactly 16 bytes of name.
    static constexpr size_t kMaxNameLength = 16;

    /// `default_name` seeds the name field; it is what `--name` supplies.
    explicit CharacterSelectModel(const std::string& default_name = "haven");

    /// Up/Down: moves the highlight between the fields, wrapping at both ends.
    void move_field(int delta);

    /// Left/Right: cycles the race or the weapon, whichever field is selected.
    /// The name and START fields have no value to change.
    void change_value(int delta);

    /// Enter. Returns true when the player is ready to connect, which is Enter
    /// on the START field; Enter on the name field commits the name and moves
    /// the highlight on to the race.
    bool confirm();

    /// A printable character typed on the name field.
    void type_char(char c);

    /// Backspace on the name field. The name never empties: a nameless player
    /// is not something the server can name in its log.
    void backspace();

    int field() const { return field_; }
    const std::string& name() const { return name_; }
    int race_id() const { return race_id_; }
    int weapon_id() const { return weapon_id_; }

private:
    bool editing_name() const { return field_ == kFieldName; }

    int field_ = kFieldName;
    std::string name_;
    int race_id_ = 0;
    int weapon_id_ = 0;
};

}  // namespace haven
