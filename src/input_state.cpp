#include "input_state.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace haven {

Vec2 InputState::move_vector() const
{
    if (script_ != nullptr) return script_index_ < script_->size() ? (*script_)[script_index_++] : Vec2{};

    const auto is_down = [this](int key) { return held_.find(key) != held_.end() && held_.at(key); };
    Vec2 v;
    if (is_down(kKeyA) || is_down(kKeyLeft)) v.x -= 1.0f;
    if (is_down(kKeyD) || is_down(kKeyRight)) v.x += 1.0f;
    if (is_down(kKeyW) || is_down(kKeyUp)) v.y += 1.0f;
    if (is_down(kKeyS) || is_down(kKeyDown)) v.y -= 1.0f;
    return v;
}

void InputState::key_down(int key_code)
{
    last_key_ = key_code;
    held_[key_code] = true;
    if (key_code == kKeyEscape) quit_ = true;
    if (key_code == kKey1) pending_choice_ = 0;
    if (key_code == kKey2) pending_choice_ = 1;
    if (key_code == kKey3) pending_choice_ = 2;
    if (key_code == kKeyR) restart_ = true;
}

void InputState::key_up(int key_code)
{
    held_[key_code] = false;
}

bool InputState::held(int key_code) const
{
    const auto it = held_.find(key_code);
    return it != held_.end() && it->second;
}

void InputState::set_script(const std::vector<Vec2>& script)
{
    script_ = &script;
    script_index_ = 0;
}

void InputState::set_choice_script(const std::vector<int8_t>& picks)
{
    choice_script_ = &picks;
    choice_index_ = 0;
}

int8_t InputState::peek_scripted_line() const
{
    if (choice_script_ == nullptr || choice_index_ >= choice_script_->size()) return -3;
    return (*choice_script_)[choice_index_];
}

int8_t InputState::take_scripted_choice()
{
    const int8_t line = peek_scripted_line();
    // Not a choice index: hand the value back untouched (-1 restart marker,
    // -3 exhausted) and leave the line for the event it names.
    if (line < 0 || line > 2) return line;
    ++choice_index_;
    return line;
}

bool InputState::take_scripted_restart()
{
    if (peek_scripted_line() != -1) return false;
    ++choice_index_;
    return true;
}

bool load_input_script(const std::string& path, std::vector<Vec2>& out)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        std::fprintf(stderr, "haven: cannot open input script %s\n", path.c_str());
        return false;
    }

    const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };

    std::string line;
    int line_number = 0;
    while (std::getline(file, line))
    {
        ++line_number;
        const size_t comment = line.find('#');
        if (comment != std::string::npos) line = line.substr(0, comment);

        size_t begin = 0;
        size_t end = line.size();
        while (begin < end && is_space(line[begin])) ++begin;
        while (end > begin && is_space(line[end - 1])) --end;
        if (begin == end) continue;  // blank line or comment only

        line = line.substr(begin, end - begin);
        std::istringstream fields(line);
        float x = 0.0f;
        float y = 0.0f;
        int ticks = 0;
        std::string trailing;
        if (!(fields >> x >> y >> ticks) || (fields >> trailing) || ticks <= 0)
        {
            std::fprintf(stderr, "haven: %s:%d is not \"<move_x> <move_y> <ticks>\"\n", path.c_str(), line_number);
            return false;
        }
        out.insert(out.end(), static_cast<size_t>(ticks), Vec2{x, y});
    }
    return !out.empty();
}

bool load_choice_script(const std::string& path, std::vector<int8_t>& out)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        std::fprintf(stderr, "haven: cannot open choice script %s\n", path.c_str());
        return false;
    }

    const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };

    std::string line;
    int line_number = 0;
    while (std::getline(file, line))
    {
        ++line_number;
        const size_t comment = line.find('#');
        if (comment != std::string::npos) line = line.substr(0, comment);

        size_t begin = 0;
        size_t end = line.size();
        while (begin < end && is_space(line[begin])) ++begin;
        while (end > begin && is_space(line[end - 1])) --end;
        if (begin == end) continue;  // blank line or comment only

        line = line.substr(begin, end - begin);
        if (line == "restart")
        {
            out.push_back(-1);
            continue;
        }

        std::istringstream fields(line);
        int value = -1;
        std::string trailing;
        if (!(fields >> value) || (fields >> trailing) || value < 0 || value > 2)
        {
            std::fprintf(stderr, "haven: %s:%d is not \"<choice_idx>\" or \"restart\"\n", path.c_str(),
                         line_number);
            return false;
        }
        out.push_back(static_cast<int8_t>(value));
    }
    return !out.empty();
}

}  // namespace haven
