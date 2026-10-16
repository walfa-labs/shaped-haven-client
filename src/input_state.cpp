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
}

void InputState::key_up(int key_code)
{
    held_[key_code] = false;
}

void InputState::set_script(const std::vector<Vec2>& script)
{
    script_ = &script;
    script_index_ = 0;
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

}  // namespace haven
