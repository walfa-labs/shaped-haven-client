#include "character_select.h"

#include "character_catalog.h"

namespace haven {

namespace {

int wrap(int value, int count)
{
    const int wrapped = value % count;
    return wrapped < 0 ? wrapped + count : wrapped;
}

}  // namespace

CharacterSelectModel::CharacterSelectModel(const std::string& default_name) : name_(default_name)
{
    if (name_.size() > kMaxNameLength) name_.resize(kMaxNameLength);
}

void CharacterSelectModel::move_field(int delta)
{
    field_ = wrap(field_ + delta, kFieldCount);
}

void CharacterSelectModel::change_value(int delta)
{
    if (field_ == kFieldRace)
        race_id_ = cycle_race(race_id_, delta);
    else if (field_ == kFieldWeapon)
        weapon_id_ = cycle_weapon(weapon_id_, delta);
}

bool CharacterSelectModel::confirm()
{
    if (field_ == kFieldName)
    {
        // The name is live as it is typed, so confirming it only means the
        // player is done with it: the highlight moves on to the race.
        field_ = kFieldRace;
        return false;
    }
    return field_ == kFieldStart;
}

void CharacterSelectModel::type_char(char c)
{
    if (!editing_name()) return;
    // Only what the wire and the bitmap font can both carry: the font draws
    // anything else as a blank, and a control byte in a name is never wanted.
    if (c < 32 || c > 126) return;
    if (name_.size() >= kMaxNameLength) return;
    name_.push_back(c);
}

void CharacterSelectModel::backspace()
{
    if (!editing_name()) return;
    if (name_.size() <= 1) return;
    name_.pop_back();
}

}  // namespace haven
