#include "game_session.h"
#include "input_state.h"
#include "renderer.h"

#include <OgreRoot.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <winsock2.h>

namespace {

/// Winsock is process-wide; refcounted so a single scope in main is enough.
struct WinsockScope {
    WinsockScope()
    {
        WSADATA data{};
        ::WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~WinsockScope() { ::WSACleanup(); }
};

void print_usage(const char* argv0)
{
    std::printf("usage: %s [--host 127.0.0.1] [--port 7777] [--name <name>]\n"
                "          [--frames <n>] [--screenshot <path>] [--verbose]\n"
                "          [--input-script <path>]\n"
                "\n"
                "  --input-script feeds move vectors from a file, one per 60 Hz tick,\n"
                "  instead of the keyboard. Each line is \"<move_x> <move_y> <ticks>\".\n",
                argv0);
}

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
    int frame_limit = 0;
    bool verbose = false;

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
        else if (arg == "--verbose") verbose = true;
        else if (arg == "--help" || arg == "-h") { print_usage(argv[0]); return 0; }
        else { print_usage(argv[0]); return 1; }
    }

    if (!screenshot.empty() && frame_limit == 0) frame_limit = 120;

    WinsockScope winsock;

    std::vector<haven::Vec2> scripted_moves;
    if (!input_script.empty() && !haven::load_input_script(input_script, scripted_moves))
        return 1;

    haven::HavenRenderer renderer;
    if (!renderer.initialise())
    {
        std::fprintf(stderr, "haven: failed to initialise Ogre\n");
        return 1;
    }

    haven::InputState input;
    renderer.attach_input(&input);
    if (!scripted_moves.empty())
    {
        input.set_script(scripted_moves);
        std::printf("haven: scripted input from %s (%zu ticks)\n", input_script.c_str(), scripted_moves.size());
    }

    haven::NetClient net;
    haven::GameSession session(renderer, net, input);
    if (!session.connect(host, port))
    {
        std::fprintf(stderr, "haven: could not connect to %s:%u\n", host.c_str(), static_cast<unsigned>(port));
        return 1;
    }

    const auto started = std::chrono::steady_clock::now();
    int frames = 0;

    renderer.run([&](float dt) {
        session.update(dt);
        ++frames;

        if (verbose && (frames % 60 == 0))
        {
            const haven::SessionStats& s = session.stats();
            std::printf("haven: frame=%d tick=%u srv_tick=%u acked=%u pred=(%.2f,%.2f) err=%.3f "
                        "interp=%zu remotes=%zu snaps=%u inputs=%u reconciles=%u key=%d move=(%.2f,%.2f)\n",
                        frames, s.client_tick, s.server_tick, s.last_acked_seq, s.predicted.x, s.predicted.y,
                        s.last_prediction_error, s.interpolation_depth, s.remote_player_count, s.snapshots_received,
                        s.inputs_sent, s.reconcile_count, input.last_key(), s.last_move.x, s.last_move.y);
        }

        if (frame_limit > 0 && frames >= frame_limit)
        {
            if (!screenshot.empty())
            {
                std::printf("haven: writing screenshot to %s\n", screenshot.c_str());
                renderer.capture_framebuffer(screenshot);
                std::printf("haven: screenshot written\n");
            }
            renderer.getRoot()->queueEndRendering();
        }
    });

    session.shutdown();
    renderer.closeApp();

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("haven: %d frames in %.2fs (%.1f fps)\n", frames, seconds, frames / (seconds > 0.0 ? seconds : 1.0));
    return 0;
}
