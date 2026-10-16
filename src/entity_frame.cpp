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

/// Ring of `segments` vertices on the unit circle, counter-clockwise starting
/// at `start`, stepping by `span / segments`. Used for closed circles, where
/// the last vertex is not repeated.
void emit_ring(PrimitiveSink& sink, Vec2 centre, float radius, float z, uint32_t segments, float start,
               float span, const Rgba& colour)
{
    for (uint32_t i = 0; i < segments; ++i)
    {
        const float a = start + span * (static_cast<float>(i) / static_cast<float>(segments));
        sink.vertex(centre.x + radius * std::cos(a), centre.y + radius * std::sin(a), z, colour);
    }
}

/// Ring for an open wedge: `segments + 1` vertices, so the last one lands
/// exactly on `start + span` instead of one step short of it.
void emit_arc_ring(PrimitiveSink& sink, Vec2 centre, float radius, float z, uint32_t segments, float start,
                   float span, const Rgba& colour)
{
    for (uint32_t i = 0; i <= segments; ++i)
    {
        const float a = start + span * (static_cast<float>(i) / static_cast<float>(segments));
        sink.vertex(centre.x + radius * std::cos(a), centre.y + radius * std::sin(a), z, colour);
    }
}

}  // namespace

void append_circle(PrimitiveSink& sink, Vec2 centre, float radius, const Rgba& colour, float z,
                    uint32_t segments)
{
    const uint32_t base = sink.base_index();
    sink.vertex(centre.x, centre.y, z, colour);
    emit_ring(sink, centre, radius, z, segments, 0.0f, kTwoPi, colour);
    for (uint32_t i = 0; i < segments; ++i)
    {
        sink.triangle(base, base + 1 + i, base + 1 + ((i + 1) % segments));
    }
}

void append_triangle(PrimitiveSink& sink, Vec2 centre, float radius, float angle, const Rgba& colour,
                     float z)
{
    // Three vertices 120 degrees apart, counter-clockwise, the first one at
    // `angle`.
    constexpr uint32_t kSides = 3;
    const uint32_t base = sink.base_index();
    for (uint32_t i = 0; i < kSides; ++i)
    {
        const float a = angle + static_cast<float>(i) * (kTwoPi / static_cast<float>(kSides));
        sink.vertex(centre.x + radius * std::cos(a), centre.y + radius * std::sin(a), z, colour);
    }
    sink.triangle(base, base + 1, base + 2);
}

void append_arc(PrimitiveSink& sink, Vec2 centre, float radius, float direction, float arc,
                const Rgba& colour, float z, uint32_t segments)
{
    const uint32_t base = sink.base_index();
    sink.vertex(centre.x, centre.y, z, colour);
    emit_arc_ring(sink, centre, radius, z, segments, direction - arc * 0.5f, arc, colour);
    for (uint32_t i = 0; i < segments; ++i)
    {
        sink.triangle(base, base + 1 + i, base + 1 + ((i + 1) % segments));
    }
}

const WeaponVisual& weapon_visual(uint8_t weapon_id)
{
    // DESIGN.md section 3, level 1: reach and arc for the two arc weapons,
    // projectile radius for the two projectile weapons. Speed and lifetime do
    // not change how a projectile is drawn; they only set the reach of the RAY
    // fallback that no phase-2 weapon uses.
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

float enemy_hp_bar_width(uint8_t archetype)
{
    // Wide enough to clear the body it sits over, bounded so a T3 bar stays
    // readable next to a swarm of T1 bars.
    const float width = enemy_radius(archetype) * 2.4f;
    return std::max(1.0f, std::min(width, 3.5f));
}

void append_enemy(PrimitiveSink& bodies, PrimitiveSink& bars, const EntityDrawItem& item)
{
    const float radius = enemy_radius(item.style);
    const Rgba colour = archetype_colour(item.style);

    if (archetype_is_circle(item.style))
        append_circle(bodies, item.pos, radius, colour, kZEnemy, kCircleSegments);
    else
        append_triangle(bodies, item.pos, radius, item.angle, colour, kZEnemy);

    // HP bar above the body, background first so the fill sits on top of it.
    const float width = enemy_hp_bar_width(item.style);
    const float height = 0.14f;
    const Vec2 centre{item.pos.x, item.pos.y + radius + 0.30f};
    const Rgba background{0.10f, 0.10f, 0.12f, 1.0f};
    const float fraction = std::max(0.0f, std::min(100.0f, static_cast<float>(item.hp_pct))) * 0.01f;

    bars.quad(centre.x, centre.y, width * 0.5f, height * 0.5f, kZHpBar, background);

    const Rgba fill{1.0f - 0.75f * fraction, 0.15f + 0.70f * fraction, 0.20f, 1.0f};
    // Left anchored so the bar drains from the right as HP drops.
    const float fill_width = width * fraction;
    const Vec2 fill_centre{centre.x - width * 0.5f + fill_width * 0.5f, centre.y};
    bars.quad(fill_centre.x, fill_centre.y, std::max(fill_width, 0.001f) * 0.5f, height * 0.5f, kZHpBar,
              fill);
}

void append_projectile(PrimitiveSink& projectiles, const EntityDrawItem& item)
{
    const WeaponVisual& weapon = weapon_visual(item.style);
    const ProjShape shape = static_cast<ProjShape>(item.shape);

    switch (shape)
    {
        case ProjShape::Arc:
        {
            // The wedge is centred on the projectile's own position and faces
            // the direction it was fired in.
            append_arc(projectiles, item.pos, weapon.reach, item.angle, weapon.arc, weapon.colour,
                      kZProjectile, arc_segments(weapon.arc));
            break;
        }
        case ProjShape::Circle:
            append_circle(projectiles, item.pos, weapon.radius, weapon.colour, kZProjectile, 10);
            break;
        case ProjShape::Ray:
        {
            // A rectangle running from the muzzle to the end of its reach.
            const float half_length = weapon.reach * 0.5f;
            projectiles.quad(item.pos.x + std::cos(item.angle) * half_length,
                             item.pos.y + std::sin(item.angle) * half_length, half_length,
                             weapon.radius, kZProjectile, weapon.colour, item.angle);
            break;
        }
    }
}

std::vector<EntityDrawItem> collect_draw_items(const InterpolationBuffer& buffer)
{
    std::vector<EntityDrawItem> items;
    const std::unordered_map<uint32_t, Vec2>& positions = buffer.latest_positions();
    const std::unordered_map<uint32_t, EntityStyle>& styles = buffer.latest_styles();
    if (positions.empty() || styles.empty()) return items;

    items.reserve(positions.size());
    for (const auto& entry : positions)
    {
        const auto style = styles.find(entry.first);
        if (style == styles.end()) continue;

        // An entity the buffer cannot bracket yet is simply not drawn this
        // frame; it appears once the older snapshot carrying it arrives.
        Vec2 pos;
        if (!buffer.sample(entry.first, pos)) continue;

        EntityDrawItem item;
        item.key = entry.first;
        item.kind = style->second.kind;
        item.pos = pos;
        item.angle = style->second.angle;
        item.style = style->second.style;
        item.hp_pct = style->second.hp_pct;
        item.shape = style->second.shape;
        items.push_back(item);
    }
    return items;
}

}  // namespace haven
