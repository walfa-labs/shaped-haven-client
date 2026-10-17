#include "character_select.h"
#include "game_session.h"
#include "input/InputManager.h"
#include "net/ITransport.h"
#include "net/SubprocessTransport.h"
#include "net/TcpTransport.h"
#include "renderer.h"

#include <OgreRoot.h>

// winsock2.h must come before windows.h: it defines _WINSOCK2API_, which is
// what stops windows.h from pulling in the conflicting winsock.h. windows.h
// also defines min/max as macros.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

/// Winsock is process-wide; refcounted so a single scope in main is enough.
struct WinsockScope {
    WinsockScope()
    {
        WSADATA data{};
        if (::WSAStartup(MAKEWORD(2, 2), &data) != 0)
        {
            std::fprintf(stderr, "haven: WSAStartup failed: %d\n", ::WSAGetLastError());
            available_ = false;
        }
    }
    ~WinsockScope()
    {
        if (available_) ::WSACleanup();
    }

    bool available() const { return available_; }

private:
    bool available_ = true;
};

/// The client's own executable directory, which is where the server is looked
/// for when --server-path is not given.
std::wstring module_directory()
{
    wchar_t buffer[MAX_PATH];
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0) return {};
    std::wstring path(buffer, length);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash + 1);
}

std::string to_utf8(const std::wstring& text)
{
    if (text.empty()) return {};
    const int length =
        ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
    return out;
}

std::string default_server_path()
{
    const std::wstring directory = module_directory();
    if (directory.empty()) return "shaped-haven-server.exe";
    return to_utf8(directory + L"shaped-haven-server.exe");
}

void print_usage(const char* argv0)
{
    std::printf("usage: %s [--host 127.0.0.1] [--port 7777] [--name <name>]\n"
                "          [--frames <n>] [--screenshot <path>] [--verbose]\n"
                "          [--input-script <path>] [--choice-script <path>]\n"
                "          [--key-script <path>] [--auto-single-player]\n"
                "          [--server-path <path>]\n"
                "\n"
                "  --input-script feeds move vectors from a file, one per 60 Hz tick,\n"
                "  instead of the keyboard. Each line is \"<move_x> <move_y> <ticks>\".\n"
                "  --choice-script answers level-ups from a file, one line per run\n"
                "  event in chronological order: a choice index 0..2 for each level-up\n"
                "  offer, or \"restart\" at each game over. The keyboard (1/2/3 to\n"
                "  pick a card, R to restart) always works and wins over the script.\n"
                "  --key-script presses keys for the menu and the character select,\n"
                "  one per line, each held for a frame and released on the next: a\n"
                "  key name (up, down, left, right, enter, escape, backspace, space)\n"
                "  or a single character. When it runs out on one of those screens\n"
                "  the client stops there, which is what lets --screenshot capture\n"
                "  the screen it left up.\n"
                "  --auto-single-player skips the menu and connects single player at\n"
                "  once, for automated runs.\n"
                "  --server-path names the executable Single Player spawns. It defaults\n"
                "  to shaped-haven-server.exe next to this client.\n",
                argv0);
}

/// Where the client is before a run exists. The menu is the only state that
/// does not need a server, so it is the only one that can be shown on startup.
enum class AppState {
    MainMenu,
    /// Picking a name, a race and a starting weapon. Needs no server either:
    /// the two ids it produces are what C2S_HELLO carries.
    CharacterSelect,
    Connecting,
    /// Waiting for the server's S2C_WELCOME after a successful TCP connect.
    AwaitingWelcome,
    Playing,
    /// The menu again, with the reason the last connect failed on it.
    ConnectionFailed,
};

}  // namespace

int main(int argc, char** argv)
{
    // The client is verified by capturing its stdout, so it must not be lost to
    // a crash: keep both streams unbuffered.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    std::string host = "127.0.0.1";
    uint16_t port = 7777;
    std::string name = "haven";
    std::string screenshot;
    std::string input_script;
    std::string choice_script;
    std::string key_script;
    std::string server_path = default_server_path();
    int frame_limit = 0;
    bool verbose = false;
    bool auto_single_player = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if (arg == "--host") host = next();
        else if (arg == "--port") port = static_cast<uint16_t>(std::atoi(next().c_str()));
        else if (arg == "--name") name = next();
        else if (arg == "--frames") frame_limit = std::atoi(next().c_str());
        else if (arg == "--screenshot") screenshot = next();
        else if (arg == "--input-script") input_script = next();
        else if (arg == "--choice-script") choice_script = next();
        else if (arg == "--key-script") key_script = next();
        else if (arg == "--server-path") server_path = next();
        else if (arg == "--verbose") verbose = true;
        else if (arg == "--auto-single-player") auto_single_player = true;
        else if (arg == "--help" || arg == "-h") { print_usage(argv[0]); return 0; }
        else { print_usage(argv[0]); return 1; }
    }

    if (!screenshot.empty() && frame_limit == 0) frame_limit = 120;

    WinsockScope winsock;
    if (!winsock.available())
    {
        std::fprintf(stderr, "haven: Winsock is unavailable, cannot connect\n");
        return 1;
    }

    std::vector<haven::Vec2> scripted_moves;
    if (!input_script.empty() && !haven::load_input_script(input_script, scripted_moves))
        return 1;

    std::vector<int8_t> scripted_choices;
    if (!choice_script.empty() && !haven::load_choice_script(choice_script, scripted_choices))
        return 1;

    std::vector<int> scripted_keys;
    if (!key_script.empty() && !haven::load_key_script(key_script, scripted_keys))
        return 1;

    haven::HavenRenderer renderer;
    if (!renderer.initialise())
    {
        std::fprintf(stderr, "haven: failed to initialise Ogre\n");
        return 1;
    }

    haven::InputManager input;
    renderer.attach_input(&input);
    if (!scripted_moves.empty())
    {
        input.set_script(scripted_moves);
        std::printf("haven: scripted input from %s (%zu ticks)\n", input_script.c_str(), scripted_moves.size());
    }
    if (!scripted_choices.empty())
    {
        input.set_choice_script(scripted_choices);
        std::printf("haven: scripted choices from %s (%zu events)\n", choice_script.c_str(),
                    scripted_choices.size());
    }
    if (!scripted_keys.empty())
    {
        input.set_key_script(scripted_keys);
        std::printf("haven: scripted keys from %s (%zu presses)\n", key_script.c_str(), scripted_keys.size());
    }

    haven::Hud hud(renderer.scene_manager(), renderer.mesh_factory(), renderer.camera_node(),
                  renderer.cel_generator(), renderer.race_meshes());

    AppState state = AppState::MainMenu;
    int menu_selection = 0;  // 0 = Single Player, 1 = Multiplayer
    std::string menu_error;
    /// The name, race and starting weapon. `--name` seeds the name field, and
    /// the two ids are 0 until the player changes them, which is what the
    /// client sent before the character select existed.
    haven::CharacterSelectModel character_select(name);
    std::unique_ptr<haven::ITransport> transport;
    std::unique_ptr<haven::GameSession> session;
    bool connect_attempted = false;
    float welcome_wait_seconds_ = 0.0f;
    constexpr float kWelcomeTimeoutSeconds = 5.0f;

    hud.show_main_menu();

    // For headless testing: auto-start single-player without waiting for input.
    if (auto_single_player)
    {
        menu_selection = 0;
        state = AppState::Connecting;
        connect_attempted = false;
    }

    // Edge detection for the menu keys: a press is a key that is held now and
    // was not held on the previous frame, so holding a key does not repeat.
    bool prev_up = false;
    bool prev_down = false;
    bool prev_left = false;
    bool prev_right = false;
    bool prev_enter = false;
    bool prev_escape = false;
    bool prev_backspace = false;

    const auto started = std::chrono::steady_clock::now();
    int frames = 0;

    /// Ends the run the way --frames does: the framebuffer first, so a
    /// --screenshot captures the frame that was on screen when it was called.
    const auto finish = [&]() {
        if (!screenshot.empty())
        {
            std::printf("haven: writing screenshot to %s\n", screenshot.c_str());
            renderer.capture_framebuffer(screenshot);
            std::printf("haven: screenshot written\n");
        }
        renderer.getRoot()->queueEndRendering();
    };

    renderer.run([&](float dt) {
        hud.tick(dt);
        input.update(dt);
        input.advance_key_script();

        // A gamepad appearing or vanishing mid-session is worth one line each.
        if (input.controller_just_added())
            std::printf("haven: gamepad connected: %s\n", input.controller_name());
        if (input.controller_just_removed())
            std::printf("haven: gamepad disconnected\n");

        switch (state)
        {
        case AppState::MainMenu:
        case AppState::ConnectionFailed:
        {
            const bool up = input.ui_up();
            const bool down = input.ui_down();
            const bool enter = input.ui_confirm();
            if (up && !prev_up)
            {
                menu_selection = (menu_selection + 1) % 2;
                hud.set_menu_selection(menu_selection);
            }
            if (down && !prev_down)
            {
                menu_selection = (menu_selection + 1) % 2;
                hud.set_menu_selection(menu_selection);
            }
            if (enter && !prev_enter)
            {
                // The choice of Single Player or Multiplayer is what the
                // Connecting state reads; the character select sits between
                // them, so `menu_selection` is left alone here.
                menu_error.clear();
                hud.clear_menu_error();
                hud.hide_main_menu();
                hud.set_character_select_name(character_select.name());
                hud.set_character_select_race(character_select.race_id());
                hud.set_character_select_weapon(character_select.weapon_id());
                hud.set_character_select_selection(character_select.field());
                hud.show_character_select();
                // Escape is the select screen's back key, not the quit key, and
                // printable keys are the name field's until the screen goes.
                input.set_escape_quits(false);
                input.set_text_capture(true);
                input.take_typed_text();
                state = AppState::CharacterSelect;
            }
            prev_up = up;
            prev_down = down;
            prev_enter = enter;
            break;
        }

        case AppState::CharacterSelect:
        {
            const bool up = input.ui_up();
            const bool down = input.ui_down();
            const bool left = input.ui_left();
            const bool right = input.ui_right();
            const bool enter = input.ui_confirm();
            const bool escape = input.ui_cancel();
            // Typing is keyboard only; the D-pad cannot spell a name.
            const bool backspace = input.held(haven::InputState::kKeyBackspace);

            if (up && !prev_up) character_select.move_field(-1);
            if (down && !prev_down) character_select.move_field(1);
            if (left && !prev_left) character_select.change_value(-1);
            if (right && !prev_right) character_select.change_value(1);
            if (backspace && !prev_backspace) character_select.backspace();
            for (const char typed : input.take_typed_text())
                character_select.type_char(typed);

            bool start = false;
            if (enter && !prev_enter) start = character_select.confirm();
            const bool go_back = escape && !prev_escape;

            prev_up = up;
            prev_down = down;
            prev_left = left;
            prev_right = right;
            prev_enter = enter;
            prev_escape = escape;
            prev_backspace = backspace;

            hud.set_character_select_selection(character_select.field());
            hud.set_character_select_name(character_select.name());
            hud.set_character_select_race(character_select.race_id());
            hud.set_character_select_weapon(character_select.weapon_id());

            if (start)
            {
                hud.hide_character_select();
                input.set_escape_quits(true);
                input.set_text_capture(false);
                state = AppState::Connecting;
                connect_attempted = false;
            }
            else if (go_back)
            {
                hud.hide_character_select();
                hud.show_main_menu();
                input.set_escape_quits(true);
                input.set_text_capture(false);
                state = AppState::MainMenu;
            }
            break;
        }

        case AppState::Connecting:
            if (!connect_attempted)
            {
                connect_attempted = true;
                menu_error.clear();
                hud.clear_menu_error();

                if (menu_selection == 0)
                {
                    std::printf("haven: starting %s\n", server_path.c_str());
                    transport = std::make_unique<haven::SubprocessTransport>(server_path);
                }
                else
                {
                    std::printf("haven: connecting to %s:%u\n", host.c_str(), static_cast<unsigned>(port));
                    transport = std::make_unique<haven::TcpTransport>();
                }

                session = std::make_unique<haven::GameSession>(renderer, *transport, input, hud);
                std::string connect_error;
                if (session->connect(host, port, character_select.name(),
                                     static_cast<uint8_t>(character_select.race_id()),
                                     static_cast<uint8_t>(character_select.weapon_id()), &connect_error))
                {
                    hud.hide_main_menu();
                    state = AppState::AwaitingWelcome;
                    welcome_wait_seconds_ = 0.0f;
                }
                else
                {
                    menu_error = connect_error;
                    state = AppState::ConnectionFailed;
                    session.reset();
                    transport->disconnect();
                    transport.reset();
                }
            }
            break;

        case AppState::AwaitingWelcome:
            session->update(dt);
            welcome_wait_seconds_ += dt;

            if (session->stats().welcomed)
            {
                state = AppState::Playing;
            }
            else if (welcome_wait_seconds_ >= kWelcomeTimeoutSeconds || !transport->isConnected())
            {
                menu_error = !transport->isConnected()
                    ? "Connection lost during handshake"
                    : "Server did not respond";
                state = AppState::ConnectionFailed;
                hud.show_main_menu();
                hud.set_menu_error(menu_error);
                session.reset();
                transport->disconnect();
                transport.reset();
            }
            break;

        case AppState::Playing:
            session->update(dt);

            if (!transport->isConnected())
            {
                menu_error = "Connection lost";
                state = AppState::ConnectionFailed;
                hud.show_main_menu();
                hud.set_menu_error(menu_error);
                session.reset();
                transport->disconnect();
                transport.reset();
                break;
            }

            ++frames;

            if (verbose && (frames % 60 == 0))
            {
                const haven::SessionStats& s = session->stats();
                std::printf("haven: frame=%d tick=%u srv_tick=%u acked=%u pred=(%.2f,%.2f) err=%.3f "
                            "interp=%zu remotes=%zu enemies=%zu projs=%zu gems=%zu snaps=%u d=%u/%u acks=%u ir=%u "
                            "stale=%u staleoffers=%u "
                            "malformed=%u inputs=%u "
                            "prewelcome=%u disconnected=%u sendfail=%u ackretry=%u reconciles=%u key=%d "
                            "move=(%.2f,%.2f) state=%u level=%u xp=%u/%u choices=%u evos=%u restarts=%u upd_ms=%.3f\n",
                            frames, s.client_tick, s.server_tick, s.last_acked_seq, s.predicted.x, s.predicted.y,
                            s.last_prediction_error, s.interpolation_depth, s.remote_player_count, s.enemy_count,
                            s.projectile_count, s.gem_count, s.snapshots_received, s.deltas_applied, s.deltas_rejected,
                            s.acks_sent, s.interest_radius, s.stale_snapshots, s.stale_offers,
                            s.malformed_packets, s.inputs_sent,
                            s.pre_welcome_steps,
                            s.disconnected_steps, s.send_failures, s.ack_retries, s.reconcile_count, input.last_key(),
                            s.last_move.x,
                            s.last_move.y, static_cast<unsigned>(s.game_state), static_cast<unsigned>(s.level),
                            static_cast<unsigned>(s.xp), static_cast<unsigned>(s.xp_next),
                            static_cast<unsigned>(s.choices_made), static_cast<unsigned>(s.evolutions_seen),
                            static_cast<unsigned>(s.restarts_sent), s.update_ms_ema);
            }

            if (frame_limit > 0 && frames >= frame_limit) finish();
            break;
        }

        // A key script drives the pre-game screens. Once it runs out on one of
        // them there is nothing left to do, so the client stops there and a
        // --screenshot captures the screen the script left up. The states a run
        // has reached are left alone: --frames ends those.
        if (input.key_script_finished() &&
            (state == AppState::MainMenu || state == AppState::CharacterSelect ||
             state == AppState::ConnectionFailed))
            finish();
    });

    if (session) session->shutdown();
    hud.shutdown();
    renderer.shutdown();
    renderer.closeApp();

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("haven: %d frames in %.2fs (%.1f fps)\n", frames, seconds, frames / (seconds > 0.0 ? seconds : 1.0));
    return 0;
}
