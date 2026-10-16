#include "game_session.h"

#include "protocol.h"

#include <chrono>
#include <cstdio>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace haven {

namespace {

constexpr int kMaxStepsPerFrame = 8;
constexpr int kConnectAttempts = 10;

}  // namespace

GameSession::GameSession(HavenRenderer& renderer, NetClient& net, InputState& input, Hud& hud)
    : renderer_(renderer), net_(net), input_(input), hud_(hud)
{
    // Stable per-process token; phase 1 never reconnects with it.
    session_token_ = 0x53484156454E31ULL;
}

bool GameSession::connect(const std::string& host, uint16_t port, const std::string& name)
{
    for (int attempt = 1; attempt <= kConnectAttempts; ++attempt)
    {
        if (net_.connect(host, port))
        {
            net_.start();
            if (!net_.send(encode_hello(session_token_, 0, name)))
                std::fprintf(stderr, "haven: could not queue C2S_HELLO\n");
            std::printf("haven: connected to %s:%u\n", host.c_str(), static_cast<unsigned>(port));
            return true;
        }
        std::printf("haven: connect attempt %d/%d failed\n", attempt, kConnectAttempts);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return false;
}

void GameSession::update(float dt_seconds)
{
    const auto update_started = std::chrono::steady_clock::now();

    // Fixed 60 Hz simulation steps, decoupled from the render rate so the
    // prediction matches the server's integrator exactly.
    accumulator_ += dt_seconds < 0.0f ? 0.0f : (dt_seconds > 0.25f ? 0.25f : dt_seconds);
    int steps = 0;
    while (accumulator_ >= kTickSeconds && steps < kMaxStepsPerFrame)
    {
        step_fixed();
        accumulator_ -= kTickSeconds;
        ++steps;
    }
    if (steps == kMaxStepsPerFrame) accumulator_ = 0.0f;

    stats_.client_tick = client_tick_;
    stats_.predicted = predictor_.position();

    drain_packets();

    // Input gating and HUD visibility follow the server's game_state, so a
    // missed S2C_LEVELUP_OFFER or S2C_GAME_OVER cannot leave the client in
    // the wrong mode.
    apply_run_state();

    if (run_state_ == RunState::LevelUp)
        resolve_levelup_choice();
    else if (run_state_ == RunState::GameOver)
        maybe_restart();

    // The interpolation clock tracks wall time so remote motion stays
    // continuous between snapshot arrivals.
    interpolation_.advance(dt_seconds);

    renderer_.set_local_player(predictor_.position());
    update_entities();
    renderer_.follow_camera(predictor_.position());
    hud_.set_run_time(run_time_ms_);
    hud_.tick(dt_seconds);

    const float elapsed_ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() -
                                                                       update_started)
                                 .count();
    stats_.update_ms_ema = stats_.update_ms_ema == 0.0f ? elapsed_ms : stats_.update_ms_ema * 0.9f + elapsed_ms * 0.1f;
}

void GameSession::step_fixed()
{
    // Nothing to predict against until the server has told us who we are.
    if (!stats_.welcomed)
    {
        ++stats_.pre_welcome_steps;
        return;
    }

    // The server has paused the simulation for a level-up draft or the run is
    // over. Input sent now would queue up and dump onto the arena the moment
    // the run resumes, and the predictor would run away from any state
    // reconciliation could correct, so hold both instead.
    if (run_state_ != RunState::Running)
    {
        ++stats_.disconnected_steps;
        return;
    }

    // Without a live link the integrator would run away from any state
    // reconciliation could correct, so hold position instead.
    if (!net_.connected())
    {
        ++stats_.disconnected_steps;
        return;
    }

    const Vec2 move = input_.move_vector();
    stats_.last_move = move;

    // Reserve the sequence number the predictor is about to use, so the packet
    // describes exactly the step that is about to be taken.
    const uint32_t seq = predictor_.next_seq();
    if (!net_.send(encode_input(seq, client_tick_, move, 0, 0)))
    {
        // The link went away between the connected() check above and the send.
        // The move is lost, so do not advance the prediction either: a step the
        // server never saw would show up as a permanent offset.
        ++stats_.send_failures;
        return;
    }

    predictor_.step(move);
    renderer_.set_local_player(predictor_.position());
    ++stats_.inputs_sent;
    ++client_tick_;
}

void GameSession::drain_packets()
{
    packets_.clear();
    net_.poll(packets_);
    for (const std::vector<uint8_t>& packet : packets_)
    {
        PacketHeader header{};
        if (!decode_header(packet.data(), packet.size(), header)) continue;
        const uint8_t* payload = packet.data() + kHeaderSize;
        const size_t payload_len = packet.size() - kHeaderSize;
        // The framing in net_client hands over exactly header + payload_len
        // bytes, so a mismatch means one of the two is lying. Decoding it
        // anyway would mean reading a record stream whose length the header
        // does not describe.
        if (header.payload_len != payload_len)
        {
            ++stats_.malformed_packets;
            continue;
        }
        if (static_cast<uint8_t>(MsgType::S2C_WELCOME) == header.msg_type)
            handle_welcome(payload, payload_len);
        else if (static_cast<uint8_t>(MsgType::S2C_SNAPSHOT_FULL) == header.msg_type)
            handle_snapshot(payload, payload_len);
        else if (static_cast<uint8_t>(MsgType::S2C_LEVELUP_OFFER) == header.msg_type)
            handle_levelup_offer(payload, payload_len);
        else if (static_cast<uint8_t>(MsgType::S2C_GAME_OVER) == header.msg_type)
            handle_game_over(payload, payload_len);
        else if (static_cast<uint8_t>(MsgType::S2C_LOADOUT) == header.msg_type)
            handle_loadout(payload, payload_len);
    }
}

void GameSession::handle_levelup_offer(const uint8_t* payload, size_t len)
{
    LevelUpOffer offer{};
    if (!decode_levelup_offer(payload, len, offer))
    {
        ++stats_.malformed_packets;
        std::fprintf(stderr, "haven: malformed S2C_LEVELUP_OFFER (%zu bytes)\n", len);
        return;
    }

    // One client drives one player: an offer for another slot is for the
    // co-op client of phase 5, not this one.
    if (offer.player_id != stats_.player_id) return;

    // The server pauses the simulation while it waits for the choice, so the
    // modal goes up now and input freezes until the run resumes.
    pending_offer_ = offer;
    run_state_ = RunState::LevelUp;
    std::printf("haven: level-up offer event=%u level=%u options=[%u %u %u] [%u %u %u] [%u %u %u]\n",
                static_cast<unsigned>(offer.event_id), static_cast<unsigned>(offer.new_level),
                static_cast<unsigned>(offer.options[0].item_id), static_cast<unsigned>(offer.options[0].next_level),
                static_cast<unsigned>(offer.options[0].is_evo), static_cast<unsigned>(offer.options[1].item_id),
                static_cast<unsigned>(offer.options[1].next_level), static_cast<unsigned>(offer.options[1].is_evo),
                static_cast<unsigned>(offer.options[2].item_id), static_cast<unsigned>(offer.options[2].next_level),
                static_cast<unsigned>(offer.options[2].is_evo));
}

void GameSession::handle_game_over(const uint8_t* payload, size_t len)
{
    GameOverStats stats{};
    if (!decode_game_over(payload, len, stats))
    {
        ++stats_.malformed_packets;
        std::fprintf(stderr, "haven: malformed S2C_GAME_OVER (%zu bytes)\n", len);
        return;
    }

    game_over_ = stats;
    run_state_ = RunState::GameOver;
    std::printf("haven: game over after %u s, score %u, kills %u\n", static_cast<unsigned>(stats.seconds_survived),
                static_cast<unsigned>(stats.score), static_cast<unsigned>(stats.kills));
}

void GameSession::handle_loadout(const uint8_t* payload, size_t len)
{
    Loadout loadout{};
    if (!decode_loadout(payload, len, loadout))
    {
        ++stats_.malformed_packets;
        std::fprintf(stderr, "haven: malformed S2C_LOADOUT (%zu bytes)\n", len);
        return;
    }

    stats_.level = loadout.level;
    stats_.xp = loadout.xp;
    stats_.xp_next = loadout.xp_next;
    hud_.set_loadout(loadout);
}

void GameSession::apply_run_state()
{
    // The snapshot header's game_state outranks the packets above: it is what
    // the simulation is actually doing right now.
    if (stats_.game_state == GameState::Running && run_state_ != RunState::Running)
    {
        pending_offer_.reset();
        run_state_ = RunState::Running;
        hud_.hide_offer();
        hud_.hide_game_over();
    }
    else if (stats_.game_state == GameState::GameOver && run_state_ != RunState::GameOver)
    {
        run_state_ = RunState::GameOver;
        hud_.show_game_over(game_over_);
    }
}

void GameSession::resolve_levelup_choice()
{
    if (!pending_offer_.has_value()) return;

    // The keyboard wins over the script: a human at the machine is the
    // manual path, the script is the unattended one.
    int pick = input_.pending_choice();
    input_.consume_choice();
    if (pick < 0 && input_.has_choice_script())
    {
        const int8_t scripted = input_.consume_scripted_line();
        if (scripted >= 0 && scripted <= 2)
            pick = scripted;
        else if (scripted == -1)
            pick = 0;  // a restart marker at an offer: server default is option 0
    }
    if (pick < 0 || pick > 2) return;  // wait for a keypress or the script

    if (!net_.send(encode_levelup_choice(pending_offer_->event_id, static_cast<uint8_t>(pick))))
    {
        ++stats_.send_failures;
        return;
    }
    ++stats_.choices_made;
    std::printf("haven: level-up choice event=%u idx=%d\n", static_cast<unsigned>(pending_offer_->event_id),
                pick);
    pending_offer_.reset();
    // The modal comes down when the next snapshot says the run is running
    // again (apply_run_state), not when the packet goes out.
}

void GameSession::maybe_restart()
{
    bool restart = input_.restart_requested();
    input_.consume_restart();

    int8_t scripted = -3;
    if (!restart && input_.has_choice_script())
    {
        scripted = input_.consume_scripted_line();
        restart = scripted == -1;
    }
    if (!restart) return;

    if (!net_.send(encode_restart()))
    {
        ++stats_.send_failures;
        return;
    }
    ++stats_.restarts_sent;
    std::printf("haven: restart requested\n");
    // The old run's entities keep their ids in the pools, so the interpolation
    // buffer and the HUD start the fresh run clean. Reconciliation snaps the
    // predictor to the respawn point on the first snapshot.
    interpolation_.clear();
    renderer_.clear_remote_players();
    hud_.reset();
    stats_.level = 1;
    stats_.xp = 0;
    stats_.xp_next = 0;
}

void GameSession::handle_welcome(const uint8_t* payload, size_t len)
{
    Welcome welcome{};
    if (!decode_welcome(payload, len, welcome))
    {
        std::fprintf(stderr, "haven: malformed S2C_WELCOME\n");
        return;
    }
    stats_.welcomed = true;
    stats_.player_id = welcome.player_id;
    std::printf("haven: welcome player_id=%u tick_hz=%u snap_hz=%u arena_half=%u server_tick=%u\n",
                static_cast<unsigned>(welcome.player_id), static_cast<unsigned>(welcome.tick_hz),
                static_cast<unsigned>(welcome.snap_hz), static_cast<unsigned>(welcome.arena_half_size),
                welcome.server_tick);
}

void GameSession::handle_snapshot(const uint8_t* payload, size_t len)
{
    SnapshotFull snapshot{};
    if (!decode_snapshot_full(payload, len, snapshot))
    {
        ++stats_.malformed_packets;
        std::fprintf(stderr, "haven: malformed S2C_SNAPSHOT_FULL (%zu bytes)\n", len);
        return;
    }

    ++stats_.snapshots_received;

    stats_.game_state = snapshot.game_state;
    run_time_ms_ = snapshot.run_time_ms;

    // Snapshots must arrive in order. A stale one carries an older
    // authoritative position, and applying it would drag the prediction
    // backwards.
    if (stats_.server_tick != 0 && snapshot.server_tick < stats_.server_tick)
    {
        ++stats_.stale_snapshots;
        return;
    }
    stats_.server_tick = snapshot.server_tick;

    // Reused, not rebuilt: clearing the maps keeps their buckets, so the only
    // allocation left in this path is the one InterpolationBuffer::push makes
    // for the copy it stores.
    RenderSnapshot& render = pending_snapshot_;
    render.positions.clear();
    render.styles.clear();
    render.run_time_ms = snapshot.run_time_ms;

    // Every stream goes into one interpolation buffer, so each is stored under
    // a key that carries its kind. Only the position is interpolated; the style
    // travels alongside and is taken from the newest snapshot.
    for (const PlayerNet& player : snapshot.players)
    {
        const uint32_t key = interpolation_key(player.id, EntityKind::Player);
        render.positions.emplace(key, player.pos);
        EntityStyle style;
        style.kind = EntityKind::Player;
        style.angle = player.vel_angle;
        render.styles.emplace(key, style);
    }

    for (const EnemyNet& enemy : snapshot.enemies)
    {
        const uint32_t key = interpolation_key(enemy.id, EntityKind::Enemy);
        render.positions.emplace(key, enemy.pos);
        EntityStyle style;
        style.kind = EntityKind::Enemy;
        style.style = enemy.archetype;
        style.hp_pct = enemy.hp_pct;
        style.angle = enemy.vel_angle;
        render.styles.emplace(key, style);
    }

    for (const ProjNet& proj : snapshot.projs)
    {
        const uint32_t key = interpolation_key(proj.id, EntityKind::Projectile);
        render.positions.emplace(key, proj.pos);
        EntityStyle style;
        style.kind = EntityKind::Projectile;
        style.style = proj.weapon_id;
        style.shape = proj.shape;
        style.angle = proj.vel_angle;
        render.styles.emplace(key, style);
    }

    for (const GemNet& gem : snapshot.gems)
    {
        const uint32_t key = interpolation_key(gem.id, EntityKind::Gem);
        render.positions.emplace(key, gem.pos);
        EntityStyle style;
        style.kind = EntityKind::Gem;
        style.style = gem_bucket(gem.xp_value);
        render.styles.emplace(key, style);
    }

    // Reconcile on every snapshot, not only when the ack advances: the server
    // can re-send the same acked sequence with a corrected position, and the
    // blend needs those corrections to converge instead of leaving residual
    // error behind on repeated identical acks.
    if (stats_.welcomed && stats_.player_id < snapshot.acked_seq.size())
    {
        const uint32_t acked = snapshot.acked_seq[stats_.player_id];
        const PlayerNet* self = nullptr;
        for (const PlayerNet& player : snapshot.players)
            if (player.id == stats_.player_id) self = &player;

        if (self != nullptr)
        {
            stats_.last_acked_seq = acked;
            stats_.authoritative = self->pos;
            const Vec2 before = predictor_.position();
            predictor_.reconcile(acked, self->pos);
            stats_.last_prediction_error = distance(before, predictor_.position());
            ++stats_.reconcile_count;
        }
    }

    interpolation_.push(render);
    stats_.interpolation_depth = interpolation_.size();
}

void GameSession::update_entities()
{
    if (!stats_.welcomed) return;

    collect_draw_items(interpolation_, draw_items_);

    // Two buffers, swapped rather than rebuilt: `present_remotes_` collects the
    // remote players seen this frame, and after the diff the two sets swap so
    // both keep the buckets the larger of them needed.
    present_remotes_.clear();
    combat_items_.clear();
    combat_items_.reserve(draw_items_.size());
    for (const EntityDrawItem& item : draw_items_)
    {
        if (item.kind != EntityKind::Player)
        {
            combat_items_.push_back(item);
            continue;
        }
        const uint32_t id = key_raw_id(item.key);
        if (id == stats_.player_id) continue;
        present_remotes_.insert(id);
        renderer_.set_remote_player(static_cast<uint8_t>(id), item.pos);
    }

    for (uint32_t id : known_remotes_)
        if (present_remotes_.find(id) == present_remotes_.end()) renderer_.remove_player(id);
    known_remotes_.swap(present_remotes_);

    renderer_.render_entities(combat_items_);

    size_t enemies = 0;
    size_t projectiles = 0;
    size_t gems = 0;
    for (const EntityDrawItem& item : combat_items_)
    {
        if (item.kind == EntityKind::Enemy) ++enemies;
        else if (item.kind == EntityKind::Projectile) ++projectiles;
        else if (item.kind == EntityKind::Gem) ++gems;
    }
    stats_.enemy_count = enemies;
    stats_.projectile_count = projectiles;
    stats_.gem_count = gems;
    stats_.remote_player_count = known_remotes_.size();
}

void GameSession::shutdown()
{
    net_.stop();
}

}  // namespace haven
