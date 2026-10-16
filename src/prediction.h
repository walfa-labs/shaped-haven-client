#pragma once

#include "game_math.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace haven {

/// One predicted step of the local player, kept so the client can re-simulate
/// everything the server has not acknowledged yet (DESIGN.md section 2).
struct InputSample {
    uint32_t seq = 0;
    Vec2 move;
    Vec2 predicted_pos;
};

/// Fixed-size ring buffer of the last 128 inputs.
class InputHistory {
public:
    static constexpr size_t kCapacity = 128;

    void push(InputSample sample);
    void clear();

    size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }

    /// Samples in increasing sequence order, oldest first.
    const InputSample& at(size_t index) const;
    const InputSample* find(uint32_t seq) const;

    /// Drops every sample at or before `acked_seq`; the server has already
    /// applied those.
    void discard_at_or_before(uint32_t acked_seq);

private:
    size_t index_of(size_t logical) const { return (head_ + logical) % kCapacity; }

    std::array<InputSample, kCapacity> samples_{};
    size_t head_ = 0;
    size_t count_ = 0;
};

/// 60 Hz local prediction plus reconciliation against the server's
/// `acked_input_seq`.
class ClientPredictor {
public:
    ClientPredictor();

    /// Applies one fixed 60 Hz step of input and records it for replay.
    Vec2 step(const Vec2& move);

    /// Re-simulates the unacknowledged inputs from the authoritative position
    /// and folds the result back into the prediction. Errors at or above
    /// kReconcileSnapThreshold snap; smaller errors blend out at
    /// kReconcileBlendRate per call.
    void reconcile(uint32_t acked_seq, const Vec2& server_pos);

    Vec2 position() const { return position_; }
    void set_position(const Vec2& pos) { position_ = pos; }
    uint32_t next_seq() const { return next_seq_; }
    uint32_t last_seq() const { return next_seq_ - 1; }
    const InputHistory& history() const { return history_; }

private:
    Vec2 position_{0.0f, 0.0f};
    uint32_t next_seq_ = 1;
    InputHistory history_;
};

}  // namespace haven
