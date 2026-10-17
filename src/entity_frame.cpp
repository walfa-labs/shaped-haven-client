#include "entity_frame.h"

#include "interpolation.h"

#include <algorithm>

namespace haven {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

/// A circle this many segments long still reads as a circle at the 80 x 45
/// viewport, and keeps the per-frame vertex count low.
constexpr uint32_t kCircleSegments = 16;

/// Segments used for a weapon arc: one per ~20 degrees, so a 90 degree sword
/// slash and a 160 degree axe sweep both come out smooth.
uint32_t arc_segments(float arc)
{
    const uint32_t segments = static_cast<uint32_t>(std::lround(std::fabs(arc) * (180.0f / kPi) / 20.0f));
    return std::max(2u, std::min(segments, 24u));
}

/// Radius of the marker drawn for a projectile whose `shape` byte this client
/// does not recognise and whose weapon carries no body radius of its own. The
/// arc weapons report a radius of zero, so a bare `weapon.radius` fallback would
/// draw nothing at all.
constexpr float kUnknownShapeRadius = 0.30f;

/// Ring of `segments` vertices on the unit circle, counter-clockwise starting
/// at `start`, stepping by `span / segments`. Used for closed circles, where
/// the last vertex is not repeated.
void emit_ring(PrimitiveSink& sink, Vec2 centre, float radius, float z, uint32_t segments, float start,
               float span)
{
    for (uint32_t i = 0; i < segments; ++i)
    {
        const float a = start + span * (static_cast<float>(i) / static_cast<float>(segments));
        sink.vertex(centre.x + radius * std::cos(a), centre.y + radius * std::sin(a), z);
    }
}

/// Ring for an open wedge: `segments + 1` vertices, so the last one lands
/// exactly on `start + span` instead of one step short of it.
void emit_arc_ring(PrimitiveSink& sink, Vec2 centre, float radius, float z, uint32_t segments, float start,
                   float span)
{
    for (uint32_t i = 0; i <= segments; ++i)
    {
        const float a = start + span * (static_cast<float>(i) / static_cast<float>(segments));
        sink.vertex(centre.x + radius * std::cos(a), centre.y + radius * std::sin(a), z);
    }
}

/// Fills an open wedge from the centre vertex at `base` into `segments`
/// triangles. The ring has `segments + 1` vertices, so the two ring indices of
/// triangle `i` are the consecutive pair `i` and `i + 1`: every interval is
/// covered exactly once and no triangle folds back onto the first ring vertex.
/// (Wrapping the second index with `% segments` — correct for a closed ring —
/// would leave the final interval uncovered and draw a stray sliver instead.)
void fan_arc(PrimitiveSink& sink, uint32_t base, uint32_t segments)
{
    for (uint32_t i = 0; i < segments; ++i)
        sink.triangle(base, base + 1 + i, base + 2 + i);
}

}  // namespace

void append_circle(PrimitiveSink& sink, Vec2 centre, float radius, float z, uint32_t segments)
{
    const uint32_t base = sink.base_index();
    sink.vertex(centre.x, centre.y, z);
    emit_ring(sink, centre, radius, z, segments, 0.0f, kTwoPi);
    for (uint32_t i = 0; i < segments; ++i)
    {
        sink.triangle(base, base + 1 + i, base + 1 + ((i + 1) % segments));
    }
}

void append_triangle(PrimitiveSink& sink, Vec2 centre, float radius, float angle, float z)
{
    // Three vertices 120 degrees apart, counter-clockwise, the first one at
    // `angle`.
    constexpr uint32_t kSides = 3;
    const uint32_t base = sink.base_index();
    for (uint32_t i = 0; i < kSides; ++i)
    {
        const float a = angle + static_cast<float>(i) * (kTwoPi / static_cast<float>(kSides));
        sink.vertex(centre.x + radius * std::cos(a), centre.y + radius * std::sin(a), z);
    }
    sink.triangle(base, base + 1, base + 2);
}

void append_arc(PrimitiveSink& sink, Vec2 centre, float radius, float direction, float arc, float z,
                uint32_t segments)
{
    const uint32_t base = sink.base_index();
    sink.vertex(centre.x, centre.y, z);
    emit_arc_ring(sink, centre, radius, z, segments, direction - arc * 0.5f, arc);
    fan_arc(sink, base, segments);
}

void append_quad(PrimitiveSink& sink, Vec2 centre, Vec2 half_extents, float angle, float z)
{
    sink.quad(centre.x, centre.y, half_extents.x, half_extents.y, z, angle);
}

void append_wireframe_rect(PrimitiveSink& sink, Vec2 centre, Vec2 half_extents, float angle, float z,
                           float thickness)
{
    const float ca = std::cos(angle);
    const float sa = std::sin(angle);
    const float t = thickness;
    const float hx = half_extents.x;
    const float hy = half_extents.y;
    // Rotates a rect-local offset into world space, so each edge is centred on
    // its rotated position and turned with the rectangle.
    const auto edge_centre = [&](float lx, float ly) {
        return Vec2{centre.x + lx * ca - ly * sa, centre.y + lx * sa + ly * ca};
    };
    // The top and bottom edges run along the rectangle's local x axis.
    const Vec2 top = edge_centre(0.0f, hy);
    sink.quad(top.x, top.y, hx, t * 0.5f, z, angle);
    const Vec2 bottom = edge_centre(0.0f, -hy);
    sink.quad(bottom.x, bottom.y, hx, t * 0.5f, z, angle);
    // The left and right edges run along its local y axis.
    const Vec2 left = edge_centre(-hx, 0.0f);
    sink.quad(left.x, left.y, t * 0.5f, hy, z, angle);
    const Vec2 right = edge_centre(hx, 0.0f);
    sink.quad(right.x, right.y, t * 0.5f, hy, z, angle);
}

const WeaponVisual& weapon_visual(uint8_t weapon_id)
{
    // DESIGN.md section 3, level 1: reach and arc for the two arc weapons,
    // projectile radius for the two projectile weapons. The server's weapon
    // table gives the bow shape RAY and the gun shape CIRCLE, so the bow's
    // `reach` is what its beam is drawn with — it is the arrow's speed times
    // its lifetime, 18 u/s x 2.0 s. The gun travels 28 u/s x 1.2 s = 33.6 u;
    // its reach only matters to the RAY fallback that nothing sends.
    static const WeaponVisual kWeapons[4] = {
        {0.0f, 3.5f, kPi * 0.5f, Rgba{1.00f, 0.78f, 0.30f, 1.0f}},  // Sword
        {0.0f, 5.0f, kPi * 0.89f, Rgba{1.00f, 0.42f, 0.35f, 1.0f}},  // Axe
        {0.40f, 36.0f, 0.0f, Rgba{0.40f, 0.90f, 1.00f, 1.0f}},       // Bow
        {0.25f, 33.6f, 0.0f, Rgba{1.00f, 0.92f, 0.35f, 1.0f}},       // Gun
    };
    const uint8_t index = weapon_id < 4 ? weapon_id : 0;
    return kWeapons[index];
}

float enemy_radius(uint8_t archetype)
{
    // DESIGN.md section 4. The boss has no phase-2 tier; it is drawn as the
    // largest circle so it never looks smaller than a T3.
    static const float kRadius[7] = {0.5f, 0.9f, 1.4f, 0.5f, 0.7f, 1.0f, 3.0f};
    const uint8_t index = archetype < 7 ? archetype : 0;
    return kRadius[index];
}

bool archetype_is_circle(uint8_t archetype)
{
    return archetype <= 2 || archetype >= 6;
}

Rgba archetype_colour(uint8_t archetype)
{
    // One hue per shape, brightened per tier so T2 and T3 read at a glance.
    static const Rgba kCircle[3] = {Rgba{0.95f, 0.45f, 0.20f, 1.0f},
                                    Rgba{0.95f, 0.30f, 0.25f, 1.0f},
                                    Rgba{0.90f, 0.20f, 0.35f, 1.0f}};
    static const Rgba kTriangle[3] = {Rgba{0.85f, 0.60f, 0.20f, 1.0f},
                                      Rgba{0.90f, 0.50f, 0.15f, 1.0f},
                                      Rgba{0.95f, 0.45f, 0.10f, 1.0f}};
    if (archetype >= 6) return Rgba{0.80f, 0.30f, 0.90f, 1.0f};
    const bool circle = archetype_is_circle(archetype);
    const uint8_t tier = circle ? archetype : archetype - 3;
    return circle ? kCircle[tier] : kTriangle[tier];
}

Rgba elite_colour(uint8_t bit)
{
    // One ring colour per modifier, chosen to read at a glance around the
    // archetype's own body colour: Vampiric red, Splitter orange, Shielded
    // blue, Haste Aura yellow.
    static const Rgba kColour[kEliteOutlineSlots] = {Rgba{0.95f, 0.25f, 0.30f, 1.0f},
                                                     Rgba{0.95f, 0.60f, 0.20f, 1.0f},
                                                     Rgba{0.35f, 0.65f, 0.95f, 1.0f},
                                                     Rgba{0.95f, 0.90f, 0.30f, 1.0f}};
    return kColour[bit < kEliteOutlineSlots ? bit : 0];
}

const char* elite_name(uint8_t bit)
{
    static const char* const kName[kEliteOutlineSlots] = {"Vampiric", "Splitter", "Shielded", "Haste Aura"};
    return kName[bit < kEliteOutlineSlots ? bit : 0];
}

bool boss_telegraph_active(uint32_t run_time_ms)
{
    const uint32_t phase = run_time_ms % kBossCycleMs;
    return phase >= kBossTelegraphStartMs && phase < kBossTelegraphEndMs;
}

float enemy_hp_bar_width(uint8_t archetype)
{
    // Wide enough to clear the body it sits over, bounded so a T3 bar stays
    // readable next to a swarm of T1 bars.
    const float width = enemy_radius(archetype) * 2.4f;
    return std::max(1.0f, std::min(width, 3.5f));
}

uint8_t hp_fill_bucket(float hp_pct)
{
    const float clamped = std::max(0.0f, std::min(100.0f, hp_pct)) * 0.01f;
    const uint8_t bucket = static_cast<uint8_t>(clamped * static_cast<float>(kHpFillBuckets));
    return bucket >= kHpFillBuckets ? kHpFillBuckets - 1 : bucket;
}

ColourSlot slot_for_body(const EntityDrawItem& item)
{
    ColourSlot slot;
    slot.pool = ColourPool::EnemyBody;
    slot.slot = item.style < 7 ? item.style : 0;
    return slot;
}

ColourSlot slot_for_bar(const EntityDrawItem& item)
{
    ColourSlot slot;
    slot.pool = ColourPool::EnemyBar;
    slot.slot = 1 + hp_fill_bucket(static_cast<float>(item.hp_pct));
    return slot;
}

ColourSlot slot_for_projectile(const EntityDrawItem& item)
{
    ColourSlot slot;
    slot.pool = ColourPool::Projectile;
    slot.slot = item.style < 4 ? item.style : 0;
    return slot;
}

ColourSlot slot_for_gem(const EntityDrawItem& item)
{
    ColourSlot slot;
    slot.pool = ColourPool::Gem;
    slot.slot = item.style < kGemSlots ? item.style : 0;
    return slot;
}

ColourSlot slot_for_elite_outline(uint8_t bit)
{
    ColourSlot slot;
    slot.pool = ColourPool::EliteOutline;
    slot.slot = bit < kEliteOutlineSlots ? bit : 0;
    return slot;
}

ColourSlot slot_for_boss_telegraph()
{
    ColourSlot slot;
    slot.pool = ColourPool::BossTelegraph;
    slot.slot = 0;
    return slot;
}

Rgba slot_colour(const ColourSlot& slot)
{
    switch (slot.pool)
    {
        case ColourPool::EnemyBody:
            return archetype_colour(slot.slot);
        case ColourPool::EnemyBar:
            if (slot.slot == 0) return Rgba{0.10f, 0.10f, 0.12f, 1.0f};
            // Drain from red at empty to green at full.
            {
                const float t = static_cast<float>(slot.slot - 1) /
                                static_cast<float>(kHpFillBuckets - 1);
                return Rgba{1.0f - 0.70f * t, 0.15f + 0.75f * t, 0.20f, 1.0f};
            }
        case ColourPool::Projectile:
            return weapon_visual(slot.slot).colour;
        case ColourPool::Gem:
            return gem_colour(slot.slot);
        case ColourPool::EliteOutline:
            return elite_colour(static_cast<uint8_t>(slot.slot));
        case ColourPool::BossTelegraph:
            // The warning-box red, distinct from every archetype and weapon.
            return Rgba{1.00f, 0.20f, 0.20f, 1.0f};
    }
    return Rgba{1.0f, 1.0f, 1.0f, 1.0f};
}

void append_enemy_body(PrimitiveSink& sink, const EntityDrawItem& item)
{
    const float radius = enemy_radius(item.style);

    if (archetype_is_circle(item.style))
        append_circle(sink, item.pos, radius, kZEnemy, kCircleSegments);
    else
        append_triangle(sink, item.pos, radius, item.angle, kZEnemy);
}

/// The dark bar the fill sits on. Own slot, because it is a different colour.
void append_enemy_bar_background(PrimitiveSink& sink, const EntityDrawItem& item)
{
    const float radius = enemy_radius(item.style);
    const float width = enemy_hp_bar_width(item.style);
    const float height = 0.14f;
    const Vec2 centre{item.pos.x, item.pos.y + radius + 0.30f};
    sink.quad(centre.x, centre.y, width * 0.5f, height * 0.5f, kZHpBar);
}

/// The fill, whose width tracks HP and whose slot tracks how full the bar is.
void append_enemy_bar(PrimitiveSink& sink, const EntityDrawItem& item)
{
    const float radius = enemy_radius(item.style);
    const float width = enemy_hp_bar_width(item.style);
    const float height = 0.14f;
    const Vec2 centre{item.pos.x, item.pos.y + radius + 0.30f};

    const float fraction = std::max(0.0f, std::min(100.0f, static_cast<float>(item.hp_pct))) * 0.01f;
    const float fill_width = width * fraction;
    // Left anchored so the bar drains from the right as HP drops.
    const Vec2 fill_centre{centre.x - width * 0.5f + fill_width * 0.5f, centre.y};
    sink.quad(fill_centre.x, fill_centre.y, std::max(fill_width, 0.001f) * 0.5f, height * 0.5f, kZHpBar);
}

void append_projectile(PrimitiveSink& sink, const EntityDrawItem& item)
{
    const WeaponVisual& weapon = weapon_visual(item.style);

    switch (static_cast<ProjShape>(item.shape))
    {
        case ProjShape::Arc:
        {
            // The wedge is centred on the projectile's own position and faces
            // the direction it was fired in.
            append_arc(sink, item.pos, weapon.reach, item.angle, weapon.arc, kZProjectile,
                      arc_segments(weapon.arc));
            return;
        }
        case ProjShape::Circle:
            // An enemy shot carries `weapon_id` 0, which is the sword — a melee
            // weapon with a radius of zero — so a zero-radius circle would draw
            // nothing and hostile fire would be invisible. Fall back to the same
            // floor the unknown-shape path uses.
            append_circle(sink, item.pos,
                          weapon.radius > 0.0f ? weapon.radius : kUnknownShapeRadius, kZProjectile, 10);
            return;
        case ProjShape::Ray:
        {
            // A rectangle running from the muzzle to the end of its reach.
            const float half_length = weapon.reach * 0.5f;
            append_quad(sink, Vec2{item.pos.x + std::cos(item.angle) * half_length,
                                   item.pos.y + std::sin(item.angle) * half_length},
                        Vec2{half_length, weapon.radius}, item.angle, kZProjectile);
            return;
        }
    }

    // A `shape` byte this client does not know: a newer server, or a corrupt
    // payload. Drawing nothing would make a live projectile invisible, which
    // reads as a desync, so fall back to the body circle. A projectile weapon
    // already has a body radius to use; the arc weapons carry a radius of zero
    // — their reach lives in `reach` — so the fallback needs a floor of its own.
    const float radius = weapon.radius > 0.0f ? weapon.radius : kUnknownShapeRadius;
    append_circle(sink, item.pos, radius, kZProjectile, 10);
}

uint8_t gem_bucket(uint16_t xp_value)
{
    // DESIGN.md section 1 gem values: Small = 2, Medium = 10, Large = 50,
    // Boss Cache = 250. An elite drop is 5x the base, so a value between the
    // named steps still lands in the bucket below it.
    if (xp_value <= 2) return 0;
    if (xp_value <= 10) return 1;
    if (xp_value <= 50) return 2;
    return 3;
}

float gem_radius(uint8_t bucket)
{
    // A gem has to read at the 80 x 45 viewport without hiding the enemy
    // swarm under it, so even the boss cache stays under half a unit.
    static const float kRadius[kGemSlots] = {0.22f, 0.27f, 0.33f, 0.42f};
    return kRadius[bucket < kGemSlots ? bucket : 0];
}

Rgba gem_colour(uint8_t bucket)
{
    // The four gem tiers of DESIGN.md section 1: small cyan, medium green,
    // large violet, boss cache gold. Value reads at a glance across a swarm.
    static const Rgba kColour[kGemSlots] = {Rgba{0.35f, 0.85f, 0.95f, 1.0f},
                                            Rgba{0.35f, 0.90f, 0.45f, 1.0f},
                                            Rgba{0.65f, 0.45f, 1.00f, 1.0f},
                                            Rgba{1.00f, 0.80f, 0.25f, 1.0f}};
    return kColour[bucket < kGemSlots ? bucket : 0];
}

void append_gem(PrimitiveSink& sink, const EntityDrawItem& item)
{
    const uint8_t bucket = item.style < kGemSlots ? item.style : 0;
    const float radius = gem_radius(bucket);
    // A diamond is a square rotated 45 degrees: `quad` takes the half extent
    // of the unrotated square, so the circumradius is extent * sqrt(2).
    constexpr float kSqrt2 = 1.41421356237f;
    sink.quad(item.pos.x, item.pos.y, radius / kSqrt2, radius / kSqrt2, kZGem, kPi * 0.25f);
}

std::vector<EntityDrawItem> collect_draw_items(const InterpolationBuffer& buffer)
{
    std::vector<EntityDrawItem> items;
    collect_draw_items(buffer, items);
    return items;
}

void collect_draw_items(const InterpolationBuffer& buffer, std::vector<EntityDrawItem>& items)
{
    // The buffer is rewritten from scratch each frame, so the caller's vector is
    // cleared rather than replaced: a steady 1,000 entity load then reuses one
    // allocation instead of taking a fresh one every frame.
    items.clear();
    const std::unordered_map<EntityKey, Vec2>& positions = buffer.latest_positions();
    const std::unordered_map<EntityKey, EntityStyle>& styles = buffer.latest_styles();
    if (positions.empty() || styles.empty()) return;

    items.reserve(positions.size());
    for (const auto& entry : positions)
    {
        const auto style = styles.find(entry.first);
        if (style == styles.end()) continue;

        // The bracketing pair the buffer interpolates over sits 100 ms behind
        // the newest snapshot, so an entity that appeared inside the last
        // snapshot interval is in neither of them. Draw it where it was when it
        // first appeared — not at its newest position, which would make it jump
        // backwards the moment the window catches up and starts interpolating
        // from that same first-seen position. Holding the first-seen position
        // makes the handover continuous.
        Vec2 pos;
        if (!buffer.sample(entry.first, pos) && !buffer.first_seen_position(entry.first, pos))
            pos = entry.second;

        EntityDrawItem item;
        item.key = entry.first;
        item.kind = style->second.kind;
        item.pos = pos;
        item.angle = style->second.angle;
        item.style = style->second.style;
        item.hp_pct = style->second.hp_pct;
        item.shape = style->second.shape;
        item.elite_mask = style->second.elite_mask;
        item.race_id = style->second.race_id;
        item.shield_pct = style->second.shield_pct;
        item.mana_pct = style->second.mana_pct;
        item.dash_cd_remaining = style->second.dash_cd_remaining;
        item.dash_cd_total = style->second.dash_cd_total;
        item.is_dashing = style->second.is_dashing;
        item.max_hp = style->second.max_hp;
        item.max_shield = style->second.max_shield;
        item.telegraph_progress = style->second.telegraph_progress;
        item.telegraph_shape = style->second.telegraph_shape;
        item.telegraph_angle = style->second.telegraph_angle;
        item.telegraph_radius = style->second.telegraph_radius;
        items.push_back(item);
    }
}

}  // namespace haven
