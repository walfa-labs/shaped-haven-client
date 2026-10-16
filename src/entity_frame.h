#pragma once

#include "game_math.h"

#include <cstdint>
#include <vector>

namespace haven {

/// An Ogre-free RGBA colour. The geometry builders emit plain data so the unit
/// tests can exercise them without linking Ogre; the renderer turns these into
/// Ogre::ColourValue when it uploads a frame.
struct Rgba {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

/// Which simulation stream an entity belongs to. Players, enemies and
/// projectiles all live in one interpolation buffer, so their ids have to be
/// told apart.
enum class EntityKind : uint8_t {
    Player = 0,
    Enemy = 1,
    Projectile = 2,
};

/// Key an entity is stored under in the interpolation buffer: the entity kind
/// in the top two bits, the server's own id below it. Every server pool is
/// orders of magnitude below 2^30 entries, so no id is ever truncated.
constexpr uint32_t kKindShift = 30;
constexpr uint32_t kKindMask = 0x3u << kKindShift;
constexpr uint32_t kRawIdMask = (1u << kKindShift) - 1u;

inline uint32_t interpolation_key(uint32_t entity_id, EntityKind kind)
{
    return (static_cast<uint32_t>(kind) << kKindShift) | (entity_id & kRawIdMask);
}

inline EntityKind key_kind(uint32_t key)
{
    return static_cast<EntityKind>((key & kKindMask) >> kKindShift);
}

inline uint32_t key_raw_id(uint32_t key)
{
    return key & kRawIdMask;
}

/// Everything the renderer needs to draw one interpolated entity for one frame.
struct EntityDrawItem {
    uint32_t key = 0;
    EntityKind kind = EntityKind::Player;
    Vec2 pos;
    float angle = 0.0f;  // facing, in radians (vel_angle decoded)
    uint8_t style = 0;   // enemy archetype, or projectile weapon_id
    uint8_t hp_pct = 0;  // enemies only; 0..100, 100 being full health
    uint8_t shape = 0;   // projectiles only; ProjNet shape byte
};

/// Immediate-mode triangle sink. `vertex()` starts a new vertex and `index()`
/// refers to vertices by insertion order. Vertices carry no colour: the colour
/// is the material's, so one section of the batch per colour holds every entity
/// drawn in that colour. The renderer uploads into a Manual Object through one
/// of these; the tests record into a plain vector.
class PrimitiveSink {
public:
    virtual ~PrimitiveSink() = default;

    /// Index the next vertex written with `vertex()`, i.e. how many vertices
    /// this sink has already emitted.
    virtual uint32_t base_index() const = 0;

    virtual void vertex(float x, float y, float z) = 0;
    virtual void index(uint32_t index) = 0;

    void triangle(uint32_t a, uint32_t b, uint32_t c)
    {
        index(a);
        index(b);
        index(c);
    }

    /// Two counter-clockwise triangles forming the rectangle centred on
    /// (cx, cy) with half extents (hx, hy), rotated by `angle`.
    void quad(float cx, float cy, float hx, float hy, float z, float angle = 0.0f)
    {
        const uint32_t base = base_index();
        const float ca = std::cos(angle);
        const float sa = std::sin(angle);
        const float corners[4][2] = {{-hx, -hy}, {hx, -hy}, {hx, hy}, {-hx, hy}};
        for (const auto& corner : corners)
        {
            const float lx = corner[0];
            const float ly = corner[1];
            vertex(cx + lx * ca - ly * sa, cy + lx * sa + ly * ca, z);
        }
        triangle(base + 0, base + 1, base + 2);
        triangle(base + 0, base + 2, base + 3);
    }
};

/// Draw layers. Everything is flat in the XY plane under a top-down
/// orthographic camera, so a distinct z per layer is the draw order.
constexpr float kZEnemy = 0.03f;
constexpr float kZProjectile = 0.04f;
constexpr float kZHpBar = 0.05f;

// ---------------------------------------------------------------------------
// Primitive builders. All of them are pure functions of their arguments.
// ---------------------------------------------------------------------------

/// Filled circle: a centre vertex plus a `segments` vertex ring and one
/// triangle per segment. Wound counter-clockwise, which Ogre treats as the
/// front face.
void append_circle(PrimitiveSink& sink, Vec2 centre, float radius, float z, uint32_t segments);

/// Equilateral triangle with circumradius `radius`; its first vertex sits at
/// `angle`, so passing a facing direction points the triangle that way.
void append_triangle(PrimitiveSink& sink, Vec2 centre, float radius, float angle, float z);

/// The wedge of a circle of `radius` centred on `direction`, spanning `arc`
/// radians symmetrically around it.
void append_arc(PrimitiveSink& sink, Vec2 centre, float radius, float direction, float arc, float z,
                uint32_t segments);

/// A rectangle rotated by `angle`, for the RAY projectile fallback.
void append_quad(PrimitiveSink& sink, Vec2 centre, Vec2 half_extents, float angle, float z);

/// The three draw layers the renderer keeps. Each is a separate Manual Object,
/// so an entity's z never has to change to be drawn in the right order.
enum class ColourPool : uint8_t {
    EnemyBody = 0,
    EnemyBar = 1,
    Projectile = 2,
};

/// One material colour inside one pool. The renderer builds one Manual Object
/// section per slot, which is how a thousand differently coloured entities still
/// cost a handful of draw calls: the colour lives in the material, not the
/// vertex.
struct ColourSlot {
    ColourPool pool = ColourPool::EnemyBody;
    uint8_t slot = 0;
};

/// Slots per pool. The bar pool holds a background colour plus one fill colour
/// per HP bucket.
constexpr uint8_t kEnemyBodySlots = 7;                  // one per archetype
constexpr uint8_t kHpFillBuckets = 8;
constexpr uint8_t kEnemyBarSlots = 1 + kHpFillBuckets;  // background + fills
constexpr uint8_t kProjectileSlots = 4;                 // one per weapon

/// The widest pool, in colour slots.
constexpr uint8_t kMaxSlotsPerPool = kEnemyBarSlots;

ColourSlot slot_for_body(const EntityDrawItem& item);
ColourSlot slot_for_bar(const EntityDrawItem& item);
ColourSlot slot_for_projectile(const EntityDrawItem& item);

/// The colour a slot is created for.
Rgba slot_colour(const ColourSlot& slot);

/// One enemy body, in the pool its archetype's colour lives in.
void append_enemy_body(PrimitiveSink& sink, const EntityDrawItem& item);

/// The dark bar an enemy's HP fill sits on, in the background slot.
void append_enemy_bar_background(PrimitiveSink& sink, const EntityDrawItem& item);

/// One enemy HP bar fill, in the pool its current fill colour lives in.
void append_enemy_bar(PrimitiveSink& sink, const EntityDrawItem& item);

/// One projectile, shaped by its ProjNet shape byte, in its weapon's pool.
void append_projectile(PrimitiveSink& sink, const EntityDrawItem& item);

// ---------------------------------------------------------------------------
// Wire-derived visual tables. The protocol carries ids only, so the client
// maps them back to the DESIGN.md tables.
// ---------------------------------------------------------------------------

/// DESIGN.md section 3 lists the weapons in this order.
enum class WeaponId : uint8_t {
    Sword = 0,
    Axe = 1,
    Bow = 2,
    Gun = 3,
};

/// DESIGN.md section 2 names the projectile shapes in this order.
enum class ProjShape : uint8_t {
    Arc = 0,
    Circle = 1,
    Ray = 2,
};

/// DESIGN.md section 3 level-1 weapon stats, as render constants. ProjNet
/// carries no geometry, so the radius and reach a projectile is drawn with are
/// these level-1 values (phase 2 runs level-1 weapons only).
struct WeaponVisual {
    float radius;  // projectile body radius
    float reach;   // arc radius and ray length
    float arc;     // arc width in radians
    Rgba colour;
};

const WeaponVisual& weapon_visual(uint8_t weapon_id);

/// DESIGN.md section 2 lists the archetypes in tier order: circles 0..2,
/// triangles 3..5, then the boss. The tier is what the radius table needs.
enum class Archetype : uint8_t {
    CircleT1 = 0,
    CircleT2 = 1,
    CircleT3 = 2,
    TriangleT1 = 3,
    TriangleT2 = 4,
    TriangleT3 = 5,
    Boss = 6,
};

float enemy_radius(uint8_t archetype);
bool archetype_is_circle(uint8_t archetype);
Rgba archetype_colour(uint8_t archetype);

/// HP bar geometry, shared by the builder and the tests.
float enemy_hp_bar_width(uint8_t archetype);

/// Bucket an HP percentage into one of the `kHpFillBuckets` fill colours, so
/// the bar drains through a fixed palette instead of a per-vertex gradient.
uint8_t hp_fill_bucket(float hp_pct);

/// Pulls the entities the newest snapshot knows about out of the interpolation
/// buffer, sampled at the render time. Only the position is interpolated; the
/// style comes from the newest snapshot that carried the entity.
std::vector<EntityDrawItem> collect_draw_items(const class InterpolationBuffer& buffer);

/// As above, but appends into a caller-owned vector so the per-frame hot path
/// does not allocate. `items` is cleared first, and its capacity is reused.
void collect_draw_items(const class InterpolationBuffer& buffer, std::vector<EntityDrawItem>& items);

}  // namespace haven
