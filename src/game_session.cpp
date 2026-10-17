#include "game_session.h"

#include "protocol.h"
#include "run_events.h"

#include <chrono>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace haven {

namespace {

constexpr int kMaxStepsPerFrame = 8;

/// Finds the client's own player record in a list of decoded player records,
/// or nullptr when the snapshot did not carry one. A stationary player can be
/// absent from a delta's updated array, and then there is nothing to
/// reconcile against this snapshot.
const PlayerNet* find_self_in(const std::vector<PlayerNet>& players, uint8_t player_id)
{
    for (const PlayerNet& p : players)
        if (p.id == player_id) return &p;
    return nullptr;
}

}  // namespace

GameSession::GameSession(HavenRenderer& renderer, ITransport& net, InputState& input, Hud& hud)
    : renderer_(renderer), net_(net), input_(input), hud_(hud)
{
    // Stable per-process token; phase 1 never reconnects with it.
    session_token_ = 0x53484156454E31ULL;
}

bool GameSession::connect(const std::string& host, uint16_t port, const std::string& name, uint8_t race_id,
                          uint8_t starting_weapon_id, std::string* outError)
{
    std::string error;
    if (!net_.connect(host, port, error))
    {
        std::fprintf(stderr, "haven: %s\n", error.c_str());
        if (outError != nullptr) *outError = error;
        return false;
    }

    const std::vector<uint8_t> hello = encode_hello(race_id, starting_weapon_id, name);
    if (!net_.sendPacket(hello.data(), hello.size()))
    {
        std::fprintf(stderr, "haven: could not send C2S_HELLO\n");
        if (outError != nullptr) *outError = "failed to send initial handshake";
        net_.disconnect();
        return false;
    }
    std::printf("haven: connected to %s:%u\n", host.c_str(), static_cast<unsigned>(port));
    std::printf("haven: C2S_HELLO name=%s race=%u weapon=%u\n", name.c_str(),
                static_cast<unsigned>(race_id), static_cast<unsigned>(starting_weapon_id));
    return true;
}

void GameSession::update(float dt_seconds)
{
    const auto update_started = std::chrono::steady_clock::now();

    // Pump whatever background I/O the transport has. For the TCP link this is
    // a no-op — NetClient's socket thread already reads and writes — but it is
    // the one call site every transport shares.
    net_.update(dt_seconds);

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

    // Retry an acknowledgement the bounded queue refused on an earlier frame,
    // so convergence does not depend on the first send happening to succeed.
    flush_ack();

    // The run state lives in the ingest, which a refused delta leaves alone;
    // mirror it for the verbose log after the packets are in.
    stats_.server_tick = ingest_.server_tick();
    stats_.game_state = ingest_.game_state();
    stats_.stale_snapshots = ingest_.stale_snapshots();

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
    hud_.set_run_time(ingest_.run_time_ms());
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
    if (!net_.isConnected())
    {
        ++stats_.disconnected_steps;
        return;
    }

    const Vec2 move = input_.move_vector();
    stats_.last_move = move;

    // Reserve the sequence number the predictor is about to use, so the packet
    // describes exactly the step that is about to be taken.
    const uint32_t seq = predictor_.next_seq();

    // Convert the normalized move vector to analog axis values for the v2 protocol.
    const auto to_i8 = [](float v) {
        return static_cast<int8_t>(std::lround(std::max(-1.0f, std::min(1.0f, v)) * 100.0f));
    };
    const int8_t axis_x = to_i8(move.x);
    const int8_t axis_y = to_i8(move.y);

    const std::vector<uint8_t> input_packet = encode_input(seq, client_tick_, 0, axis_x, axis_y);
    if (!net_.sendPacket(input_packet.data(), input_packet.size()))
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
    // One buffer large enough for the largest packet the protocol allows (a
    // 7 byte header plus a u16 payload length), so a read never splits a packet.
    uint8_t buffer[kHeaderSize + 0x10000];
    for (;;)
    {
        const size_t n = net_.receiveBytes(buffer, sizeof(buffer));
        if (n == 0) break;
        packets_.emplace_back(buffer, buffer + n);
    }
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
        else if (static_cast<uint8_t>(MsgType::S2C_SNAPSHOT_DELTA) == header.msg_type)
            handle_delta(payload, payload_len);
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

    // A duplicate, an offer older than one already answered, or a second offer
    // arriving while one is still open would reopen or replace the modal for an
    // event that is not resolved yet, and let a second choice go out for it.
    if (!levelup_gate_.accept(offer.event_id))
    {
        ++stats_.stale_offers;
        std::printf("haven: ignoring level-up offer event=%u, already seen or still open\n",
                    static_cast<unsigned>(offer.event_id));
        return;
    }

    // The server pauses the simulation while it waits for the choice, so the
    // modal goes up now and input freezes until the run resumes.
    pending_offer_ = offer;
    run_state_ = RunState::LevelUp;
    offer_tick_ = ingest_.server_tick();
    script_mismatch_reported_ = false;
    hud_.show_offer(offer);
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
    game_over_tick_ = ingest_.server_tick();
    script_mismatch_reported_ = false;
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

    // An evolution is visible when it happens: the watch reports the weapon
    // slot whose `evolved` byte just became 1. It is cleared with the run, so
    // an evolution in the new run is announced even when the slot index already
    // held one when the previous run ended.
    const int raised = evolution_watch_.observe(loadout);
    if (raised >= 0)
    {
        hud_.show_evolution(loadout.weapons[static_cast<size_t>(raised)].item_id);
        ++stats_.evolutions_seen;
    }

    hud_.set_loadout(loadout);
}

void GameSession::apply_run_state()
{
    // The snapshot header's game_state outranks the packets above when it
    // describes a newer moment of the run. A snapshot that predates the last
    // transition is a leftover from before it: the server pauses the
    // simulation the instant it sends the level-up offer, so the first
    // snapshot arriving after the offer can still carry the pre-pause
    // game_state, and acting on it would cancel the offer that just opened.
    //
    // A tick of zero means the transition has not happened in this run, and a
    // restart clears both back to zero. The server restarts its tick counter
    // with the run, so without that a new run's low ticks would keep comparing
    // at-or-below the dead run's final tick: the scoreboard would stay up and
    // input would stay frozen for the whole of the new run.
    const bool stale_after_offer = run_state_ == RunState::LevelUp && offer_tick_ != 0 &&
                                   ingest_.server_tick() <= offer_tick_;
    const bool stale_after_game_over = run_state_ == RunState::GameOver && game_over_tick_ != 0 &&
                                       ingest_.server_tick() <= game_over_tick_;

    if (ingest_.game_state() == GameState::Running && run_state_ != RunState::Running && !stale_after_offer &&
        !stale_after_game_over)
    {
        pending_offer_.reset();
        // The run is running again, so any offer that was open has been
        // answered — by the client, or by the server's 15 s timeout — and a
        // later event may open from here.
        levelup_gate_.resolve();
        // The run running again is also the first proof that a queued restart
        // went through, so the next game over may queue its own.
        restart_gate_.clear();
        run_state_ = RunState::Running;
        hud_.hide_offer();
        hud_.hide_game_over();
    }
    else if (ingest_.game_state() == GameState::GameOver && run_state_ != RunState::GameOver &&
             !stale_after_offer)
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
        // Only a line that names a level-up is spent here. A restart marker is
        // left for the game over it was written for: consuming it as choice 0
        // would both answer this offer with a pick nobody made and leave the
        // run without a restart when the run actually ends.
        const int8_t scripted = input_.take_scripted_choice();
        if (scripted >= 0 && scripted <= 2) pick = scripted;
        else if (scripted == -1) report_script_mismatch("a level-up offer", "restart");
    }
    if (pick < 0 || pick > 2) return;  // wait for a keypress or the script

    const std::vector<uint8_t> choice = encode_levelup_choice(pending_offer_->event_id, static_cast<uint8_t>(pick));
    if (!net_.sendPacket(choice.data(), choice.size()))
    {
        ++stats_.send_failures;
        return;
    }
    ++stats_.choices_made;
    // The offer is answered, so the next event may open a modal even before the
    // run resumes: the server may have already resolved it.
    levelup_gate_.resolve();
    std::printf("haven: level-up choice event=%u idx=%d\n", static_cast<unsigned>(pending_offer_->event_id),
                pick);
    pending_offer_.reset();
    // The modal comes down when the next snapshot says the run is running
    // again (apply_run_state), not when the packet goes out.
}

void GameSession::report_script_mismatch(const char* event, const char* marker)
{
    if (script_mismatch_reported_) return;
    script_mismatch_reported_ = true;
    std::fprintf(stderr,
                 "haven: choice script is out of order at %s: its next line is a `%s` marker, so the run "
                 "will wait here rather than spend it on the wrong event\n",
                 event, marker);
}

void GameSession::maybe_restart()
{
    bool script_mismatch = false;
    const bool queued = restart_gate_.take(input_, script_mismatch);
    if (script_mismatch) report_script_mismatch("the game over", "choice");
    if (!queued) return;

    const std::vector<uint8_t> restart = encode_restart();
    if (!net_.sendPacket(restart.data(), restart.size()))
    {
        // The request never went out, so this game over is still unanswered and
        // the next frame is free to try again.
        restart_gate_.clear();
        ++stats_.send_failures;
        return;
    }
    ++stats_.restarts_sent;
    std::printf("haven: restart requested\n");
    // The old run's entities keep their ids in the pools, so the interpolation
    // buffer and the HUD start the fresh run clean. Reconciliation snaps the
    // predictor to the respawn point on the first snapshot.
    interpolation_.clear();
    // The held baseline is reset with the run too: the server restarts its
    // tick counter, so the old run's baseline tick names nothing in the new
    // run, and the new run's first snapshot is a full one.
    snapshot_state_.reset();
    renderer_.clear_remote_players();
    hud_.reset();
    evolution_watch_.reset();
    // An acknowledgement still owed names a tick of the dead run, which the
    // server forgets on a restart; sending it there could drag the new run's
    // baseline onto a dead-run tick, so the debt is dropped with the run. The
    // new run's first snapshot acknowledges itself.
    ack_retry_.clear();
    // The server renumbers its offers from 1 with the run, so the gate starts
    // over too; otherwise the new run's first offer would look like an old one.
    levelup_gate_.reset();
    // The dead run's snapshots still in flight carry its final run clock and
    // its game_over state, and the new run's restart the clock at zero and
    // report the run running again. Either is proof the boundary can disarm on;
    // see run_events.h. The tick counters below are reset with it because the
    // server restarts those too.
    ingest_.begin_run(ingest_.run_time_ms());
    offer_tick_ = 0;
    game_over_tick_ = 0;
    pending_offer_.reset();
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
    stats_.interest_radius = welcome.interest_radius;
    std::printf("haven: welcome player_id=%u tick_hz=%u snap_hz=%u arena_half=%u server_tick=%u interest_radius=%u\n",
                static_cast<unsigned>(welcome.player_id), static_cast<unsigned>(welcome.tick_hz),
                static_cast<unsigned>(welcome.snap_hz), static_cast<unsigned>(welcome.arena_half_size),
                welcome.server_tick, static_cast<unsigned>(welcome.interest_radius));
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
    ++stats_.snapshots_full;

    // A full snapshot is the server's answer to a client with no usable
    // baseline: it replaces the held state outright and its tick becomes the
    // new baseline the next delta is diffed against. A full snapshot always
    // matches whatever the client holds, so the header is never refused for a
    // baseline reason.
    if (!ingest_header(snapshot.server_tick, snapshot.run_time_ms, snapshot.game_state, /*baseline_ok=*/true,
                       /*is_full=*/true))
        return;

    send_ack(snapshot_state_.apply_full(snapshot));
    reconcile_from(find_self_in(snapshot.players, stats_.player_id), snapshot.acked_seq);
    push_render(/*replacing=*/true);
}

void GameSession::handle_delta(const uint8_t* payload, size_t len)
{
    SnapshotDelta delta{};
    if (!decode_snapshot_delta(payload, len, delta))
    {
        ++stats_.malformed_packets;
        std::fprintf(stderr, "haven: malformed S2C_SNAPSHOT_DELTA (%zu bytes)\n", len);
        return;
    }

    ++stats_.snapshots_received;
    ++stats_.snapshots_delta;

    // The baseline verdict comes first, before anything with a side effect.
    // PROTOCOL.md "Baseline rules": a delta whose baseline_tick is not the tick
    // the client holds must not advance any run state at all — not the HUD
    // clock, not the game-state gating, not prediction. So the header is not
    // ingested, nothing is reconciled and nothing is rendered; the client only
    // re-acknowledges the tick it does hold, which is what corrects a drifted
    // server model and stops it refusing forever.
    if (!snapshot_state_.matches_baseline(delta.baseline_tick))
    {
        ++stats_.deltas_rejected;
        if (snapshot_state_.baseline().has_value()) send_ack(*snapshot_state_.baseline());
        return;
    }

    if (!ingest_header(delta.server_tick, delta.run_time_ms, delta.game_state, /*baseline_ok=*/true,
                       /*is_full=*/false))
        return;

    // The baseline matched, so the apply cannot fail; the tick it returns is
    // the one to acknowledge.
    uint32_t applied_baseline = 0;
    snapshot_state_.apply_delta(delta, applied_baseline);
    send_ack(applied_baseline);
    ++stats_.deltas_applied;

    // The delta was applied, so its own record of the local player is state the
    // client now holds and the prediction may be folded onto it.
    const PlayerNet* self = find_self_in(delta.spawned_players, stats_.player_id);
    if (self == nullptr) self = find_self_in(delta.updated_players, stats_.player_id);
    reconcile_from(self, delta.acked_seq);

    push_render(/*replacing=*/false);
}

bool GameSession::ingest_header(uint32_t server_tick, uint32_t run_time_ms, GameState game_state, bool baseline_ok,
                               bool is_full)
{
    // The run state and the restart boundary both live in the ingest, and its
    // offer() is the one place a snapshot header can move either. A refused
    // delta (`baseline_ok` false) returns without touching them; see
    // snapshot_ingest.h for why the ordering matters.
    return ingest_.offer(server_tick, run_time_ms, game_state, baseline_ok, is_full);
}

void GameSession::send_ack(uint32_t tick)
{
    ack_retry_.request(tick);
    flush_ack();
}

void GameSession::flush_ack()
{
    const std::optional<uint32_t> tick = ack_retry_.pending();
    if (!tick.has_value()) return;

    const std::vector<uint8_t> ack = encode_ack_snapshot(*tick);
    if (net_.sendPacket(ack.data(), ack.size()))
    {
        ack_retry_.sent();
        ++stats_.acks_sent;
    }
    else
    {
        // The bounded queue refused it. The tick stays pending so a later frame
        // retries: a forgotten acknowledgement would leave the server's model
        // of this client stuck on a tick it has already moved past.
        ++stats_.ack_retries;
    }
}

void GameSession::reconcile_from(const PlayerNet* self, const std::array<uint32_t, kMaxPlayers>& acked_seq)
{
    // Reconcile on every snapshot, not only when the ack advances: the server
    // can re-send the same acked sequence with a corrected position, and the
    // blend needs those corrections to converge instead of leaving residual
    // error behind on repeated identical acks.
    if (!stats_.welcomed || self == nullptr) return;
    if (stats_.player_id >= acked_seq.size()) return;

    const uint32_t acked = acked_seq[stats_.player_id];
    stats_.last_acked_seq = acked;
    stats_.authoritative = self->pos;
    const Vec2 before = predictor_.position();
    predictor_.reconcile(acked, self->pos);
    stats_.last_prediction_error = distance(before, predictor_.position());
    ++stats_.reconcile_count;
}

void GameSession::push_render(bool replacing)
{
    // The held state is the complete world within the interest radius, so it
    // is what goes into the interpolation buffer: an entity the server
    // destroyed is simply absent from the newest snapshot, and the draw-item
    // collection — which iterates the newest snapshot's positions — then does
    // not draw it. Nothing stale survives a destroy.
    const RenderSnapshot render = snapshot_state_.build_render(ingest_.run_time_ms());
    if (replacing)
        interpolation_.push_replacing(render);
    else
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

    renderer_.render_entities(combat_items_, ingest_.run_time_ms());

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
    net_.disconnect();
}

}  // namespace haven
