#include "prediction.h"

#include "protocol.h"

namespace haven {

void InputHistory::push(InputSample sample)
{
    if (count_ < kCapacity)
    {
        samples_[index_of(count_++)] = sample;
    }
    else
    {
        samples_[head_] = sample;
        head_ = (head_ + 1) % kCapacity;
    }
}

void InputHistory::clear()
{
    head_ = 0;
    count_ = 0;
}

const InputSample& InputHistory::at(size_t index) const
{
    return samples_[index_of(index)];
}

const InputSample* InputHistory::find(uint32_t seq) const
{
    for (size_t i = 0; i < count_; ++i)
    {
        if (at(i).seq == seq) return &at(i);
    }
    return nullptr;
}

void InputHistory::discard_at_or_before(uint32_t acked_seq)
{
    size_t drop = 0;
    while (drop < count_ && seq_at_or_before(at(drop).seq, acked_seq))
        ++drop;
    if (drop == 0) return;
    head_ = (head_ + drop) % kCapacity;
    count_ -= drop;
}

ClientPredictor::ClientPredictor() = default;

Vec2 ClientPredictor::step(const Vec2& move)
{
    const uint32_t seq = next_seq_++;
    position_ = integrate(position_, move, kTickSeconds);
    history_.push(InputSample{seq, move, position_});
    return position_;
}

void ClientPredictor::reconcile(uint32_t acked_seq, const Vec2& server_pos)
{
    // Replay from the authoritative position: the server has already applied
    // every input at or before acked_seq, so only the newer ones are replayed.
    Vec2 replayed = server_pos;
    for (size_t i = 0; i < history_.size(); ++i)
    {
        const InputSample& sample = history_.at(i);
        if (seq_at_or_before(sample.seq, acked_seq)) continue;
        replayed = integrate(replayed, sample.move, kTickSeconds);
    }

    const float error = distance(position_, replayed);
    if (error >= kReconcileSnapThreshold)
    {
        position_ = replayed;
    }
    else if (error > 0.0f)
    {
        position_ = lerp(position_, replayed, kReconcileBlendRate);
    }

    history_.discard_at_or_before(acked_seq);
}

}  // namespace haven
