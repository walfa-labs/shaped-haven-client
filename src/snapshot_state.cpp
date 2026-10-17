#include "snapshot_state.h"

namespace haven {

namespace {

/// Inserts or overwrites one entity in the held state, keyed by its namespaced
/// id. The style mapping is what the snapshot's arrays mean downstream: a gem's
/// style is its XP bucket, an enemy's its archetype, a projectile's its weapon
/// id.
void upsert_player(std::unordered_map<EntityKey, EntityDrawItem>& held, const PlayerNet& p)
{
    EntityDrawItem item;
    item.key = interpolation_key(p.id, EntityKind::Player);
    item.kind = EntityKind::Player;
    item.pos = p.pos;
    item.angle = p.vel_angle;
    item.hp_pct = p.hp_pct;
    item.race_id = p.race_id;
    item.shield_pct = p.shield_pct;
    item.mana_pct = p.mana_pct;
    item.dash_cd_remaining = p.dash_cd_remaining;
    item.dash_cd_total = p.dash_cd_total;
    item.is_dashing = p.is_dashing;
    item.max_hp = p.max_hp;
    item.max_shield = p.max_shield;
    held[item.key] = item;
}

void upsert_enemy(std::unordered_map<EntityKey, EntityDrawItem>& held, const EnemyNet& e)
{
    EntityDrawItem item;
    item.key = interpolation_key(e.id, EntityKind::Enemy);
    item.kind = EntityKind::Enemy;
    item.pos = e.pos;
    item.angle = e.vel_angle;
    item.style = e.archetype;
    item.hp_pct = e.hp_pct;
    item.elite_mask = e.elite_mask;
    item.telegraph_progress = e.telegraph_progress;
    item.telegraph_shape = e.telegraph_shape;
    item.telegraph_angle = e.telegraph_angle;
    item.telegraph_radius = e.telegraph_radius;
    held[item.key] = item;
}

void upsert_proj(std::unordered_map<EntityKey, EntityDrawItem>& held, const ProjNet& p)
{
    EntityDrawItem item;
    item.key = interpolation_key(p.id, EntityKind::Projectile);
    item.kind = EntityKind::Projectile;
    item.pos = p.pos;
    item.angle = p.vel_angle;
    item.style = p.weapon_id;
    item.shape = p.shape;
    held[item.key] = item;
}

void upsert_gem(std::unordered_map<EntityKey, EntityDrawItem>& held, const GemNet& g)
{
    EntityDrawItem item;
    item.key = interpolation_key(g.id, EntityKind::Gem);
    item.kind = EntityKind::Gem;
    item.pos = g.pos;
    item.style = gem_bucket(g.xp_value);
    held[item.key] = item;
}

void destroy_entity(std::unordered_map<EntityKey, EntityDrawItem>& held, EntityKind kind, uint32_t id)
{
    held.erase(interpolation_key(id, kind));
}

}  // namespace

uint32_t SnapshotState::apply_full(const SnapshotFull& snapshot)
{
    // Reused, not rebuilt: clearing the map keeps its buckets, so the only
    // allocation left in this path is the one a growing map needs.
    entities_.clear();
    entities_.reserve(snapshot.players.size() + snapshot.enemies.size() + snapshot.projs.size() +
                      snapshot.gems.size());

    for (const PlayerNet& p : snapshot.players) upsert_player(entities_, p);
    for (const EnemyNet& e : snapshot.enemies) upsert_enemy(entities_, e);
    for (const ProjNet& p : snapshot.projs) upsert_proj(entities_, p);
    for (const GemNet& g : snapshot.gems) upsert_gem(entities_, g);

    baseline_ = snapshot.server_tick;
    return snapshot.server_tick;
}

bool SnapshotState::apply_delta(const SnapshotDelta& delta, uint32_t& new_baseline)
{
    // A delta is applied on top of the held state only when its baseline_tick
    // is the tick that state is complete for. Anything else names a state this
    // client does not have, and applying it would corrupt the world.
    if (!matches_baseline(delta.baseline_tick)) return false;

    // Spawned and updated both upsert: an update for an id the held state does
    // not carry is the server telling us about an entity we are missing, and
    // inserting it is the self-healing thing to do. Destroyed erases.
    for (const PlayerNet& p : delta.spawned_players) upsert_player(entities_, p);
    for (const EnemyNet& e : delta.spawned_enemies) upsert_enemy(entities_, e);
    for (const ProjNet& p : delta.spawned_projs) upsert_proj(entities_, p);
    for (const GemNet& g : delta.spawned_gems) upsert_gem(entities_, g);

    for (const PlayerNet& p : delta.updated_players) upsert_player(entities_, p);
    for (const EnemyNet& e : delta.updated_enemies) upsert_enemy(entities_, e);
    for (const ProjNet& p : delta.updated_projs) upsert_proj(entities_, p);
    for (const GemNet& g : delta.updated_gems) upsert_gem(entities_, g);

    for (uint32_t id : delta.destroyed_players) destroy_entity(entities_, EntityKind::Player, id);
    for (uint32_t id : delta.destroyed_enemies) destroy_entity(entities_, EntityKind::Enemy, id);
    for (uint32_t id : delta.destroyed_projs) destroy_entity(entities_, EntityKind::Projectile, id);
    for (uint32_t id : delta.destroyed_gems) destroy_entity(entities_, EntityKind::Gem, id);

    new_baseline = delta.server_tick;
    baseline_ = delta.server_tick;
    return true;
}

bool SnapshotState::contains(uint32_t id, EntityKind kind) const
{
    return entities_.find(interpolation_key(id, kind)) != entities_.end();
}

RenderSnapshot SnapshotState::build_render(uint32_t run_time_ms) const
{
    RenderSnapshot render;
    render.run_time_ms = run_time_ms;
    for (const auto& entry : entities_)
    {
        const EntityDrawItem& item = entry.second;
        render.positions.emplace(item.key, item.pos);
        EntityStyle style;
        style.kind = item.kind;
        style.style = item.style;
        style.hp_pct = item.hp_pct;
        style.shape = item.shape;
        style.angle = item.angle;
        style.elite_mask = item.elite_mask;
        style.race_id = item.race_id;
        style.shield_pct = item.shield_pct;
        style.mana_pct = item.mana_pct;
        style.dash_cd_remaining = item.dash_cd_remaining;
        style.dash_cd_total = item.dash_cd_total;
        style.is_dashing = item.is_dashing;
        style.max_hp = item.max_hp;
        style.max_shield = item.max_shield;
        style.telegraph_progress = item.telegraph_progress;
        style.telegraph_shape = item.telegraph_shape;
        style.telegraph_angle = item.telegraph_angle;
        style.telegraph_radius = item.telegraph_radius;
        render.styles.emplace(item.key, style);
    }
    return render;
}

}  // namespace haven
