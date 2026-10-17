#include "renderer.h"

#include "render/TelegraphRenderer.h"

#include <OgreEntity.h>
#include <OgreGpuProgramManager.h>
#include <OgreHardwareBuffer.h>
#include <OgreMaterialManager.h>
#include <OgreMeshManager.h>
#include <OgrePass.h>
#include <OgreRenderOperation.h>
#include <OgreTechnique.h>
#include <OgreResourceGroupManager.h>
#include <OgreRoot.h>
#include <OgreViewport.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace haven {

namespace {

constexpr const char* kResourceGroup = "ShapedHaven";

/// DESIGN.md section 1: the orthographic viewport covers 80 x 45 units.
constexpr float kViewportWidth = 80.0f;
constexpr float kViewportHeight = 45.0f;

constexpr float kGridCell = 5.0f;

/// How many enemies, projectiles and gems the per-frame batches are sized for.
/// These are the DESIGN.md section 2 pool caps, so even a pool where every
/// entity lands in one colour never makes a slot grow mid-frame; ManualObject
/// writes no further than its estimate, so exceeding it would corrupt the
/// buffer rather than extend it.
constexpr uint32_t kMaxEnemies = 4096;
constexpr uint32_t kMaxProjectiles = 2048;
constexpr uint32_t kMaxGems = 4096;

/// Vertices and indices per entity: a 16 segment circle fan is the worst case
/// for a body, two quads make up an HP bar, a projectile is worst as an arc
/// at the 24 segment cap `arc_segments()` applies — a centre vertex plus
/// `segments + 1` ring vertices, i.e. 26 — and a gem is one diamond quad.
constexpr uint32_t kVertsPerEnemyBody = 17;
constexpr uint32_t kVertsPerEnemyBar = 8;
constexpr uint32_t kIndicesPerEnemyBody = 48;
constexpr uint32_t kIndicesPerEnemyBar = 12;
constexpr uint32_t kVertsPerProjectile = 26;
constexpr uint32_t kIndicesPerProjectile = 72;
constexpr uint32_t kVertsPerGem = 4;
constexpr uint32_t kIndicesPerGem = 6;

// Elite outline rings and boss telegraph boxes. A ring is one 16-segment
// circle, worst case one per enemy in a slot; a telegraph box is four thin
// quads, worst case one per boss in the arena's interest radius.
constexpr uint32_t kVertsPerEliteRing = 17;
constexpr uint32_t kIndicesPerEliteRing = 48;
constexpr uint32_t kMaxBossesDrawn = 64;
constexpr uint32_t kVertsPerTelegraph = 16;
constexpr uint32_t kIndicesPerTelegraph = 24;

Ogre::ColourValue colour(float r, float g, float b)
{
    return Ogre::ColourValue(r, g, b);
}

Ogre::ColourValue colour(const Rgba& rgba)
{
    return Ogre::ColourValue(rgba.r, rgba.g, rgba.b, rgba.a);
}

/// The ink outline colour for player characters: a near-black blue (#0A0A0F).
/// Elites and bosses would use a crimson ink (#FF0033); they are drawn as
/// flat batches rather than cel-shaded meshes, and their elite outline rings
/// already carry the crimson accent.
const Ogre::ColourValue kInkNormal(0.04f, 0.04f, 0.06f);

}  // namespace

HavenRenderer::HavenRenderer() : OgreBites::ApplicationContext("Shaped Haven") {}

bool HavenRenderer::initialise()
{
    initApp();
    return mRoot != nullptr && !mWindows.empty() && scene_manager_ != nullptr;
}

void HavenRenderer::createRoot()
{
    // No plugins.cfg / ogre.cfg on disk: the only plugin we need is installed
    // directly, and the render system is chosen in one_time_config().
    mRoot = new Ogre::Root("", "", "shaped-haven.log");
    mRoot->installPlugin(new Ogre::GL3PlusPlugin());
    Ogre::STBIImageCodec::startup();
}

bool HavenRenderer::oneTimeConfig()
{
    const Ogre::RenderSystemList& systems = mRoot->getAvailableRenderers();
    if (systems.empty())
    {
        std::fprintf(stderr, "haven: no Ogre render systems available\n");
        return false;
    }
    mRoot->setRenderSystem(systems.front());
    return true;
}

void HavenRenderer::locateResources()
{
    // Nothing is loaded from disk except the RTShaderSystem's own shader
    // headers, which it needs to emulate the fixed-function pipeline that the
    // GL3Plus render system no longer provides natively.
    const Ogre::String media = OGRE_MEDIA_DIR_STRING;
    Ogre::ResourceGroupManager& rgm = Ogre::ResourceGroupManager::getSingleton();
    rgm.addResourceLocation(media + "/Main", "FileSystem", "OgreInternal");
    rgm.addResourceLocation(media + "/RTShaderLib", "FileSystem", "OgreInternal");
    rgm.createResourceGroup(kResourceGroup);
}

void HavenRenderer::loadResources() {}

OgreBites::NativeWindowPair HavenRenderer::createWindow(const Ogre::String& name, uint32_t /*w*/, uint32_t /*h*/,
                                                        Ogre::NameValuePairList miscParams)
{
    // 80 x 45 units is 16:9; matching the window aspect ratio keeps the
    // orthographic projection free of stretching.
    return OgreBites::ApplicationContext::createWindow(name, 1280, 720, std::move(miscParams));
}

void HavenRenderer::setup()
{
    OgreBites::ApplicationContext::setup();

    if (!initialiseRTShaderSystem())
    {
        std::fprintf(stderr, "haven: initialiseRTShaderSystem failed\n");
        return;
    }

    scene_manager_ = mRoot->createSceneManager("DefaultSceneManager");
    mShaderGenerator->addSceneManager(scene_manager_);
    scene_manager_->setAmbientLight(colour(0.35f, 0.35f, 0.4f));

    build_scene();
}

void HavenRenderer::build_scene()
{
    meshes_ = std::make_unique<MeshFactory>(scene_manager_);

    // Player collider is 1.0 x 1.0 units, i.e. a 0.5 unit half-extent.
    meshes_->build(Shape::Square, 0.5f, "haven/player_square");
    meshes_->build(Shape::Circle, 0.5f, "haven/enemy_circle");
    meshes_->build(Shape::Triangle, 0.5f, "haven/enemy_triangle");
    meshes_->flat_material("haven/grid", colour(0.18f, 0.20f, 0.26f));
    meshes_->flat_material("haven/arena_wall", colour(0.55f, 0.35f, 0.85f));
    // White, unlit, and tracking the per-vertex colour: the combat batches put
    // an entity's whole colour into the vertex, so one material serves every
    // enemy and every projectile. The tracking flag has to be set before
    // load(): the RTShaderSystem generates a material's shader techniques at
    // load time, and it decides then whether to feed the colour attribute
    // through to the shader. Self-illumination stays black because the FFP
    // shader adds it on top of the vertex-coloured diffuse; a white
    // self-illumination would wash every entity out to white.
    //
    // This shared material is the fallback for the combat batches when the
    // cel-shading shaders failed to compile; when they did, the per-slot
    // toon materials (created in create_slot) replace it pool by pool.
    Ogre::MaterialPtr batch = Ogre::MaterialManager::getSingleton().create("haven/batch", kResourceGroup);
    Ogre::Pass* batch_pass = batch->getTechnique(0)->getPass(0);
    const Ogre::ColourValue white(1.0f, 1.0f, 1.0f);
    batch_pass->setAmbient(white);
    batch_pass->setDiffuse(white);
    batch_pass->setSelfIllumination(Ogre::ColourValue::Black);
    batch_pass->setVertexColourTracking(Ogre::TVC_DIFFUSE);
    batch->load();

    // Cel-shading: the shader programs and the procedural race meshes. If the
    // shaders fail to compile, cel_ready_ stays false and every draw path
    // falls back to the flat materials above, so the game stays playable.
    cel_generator_ = std::make_unique<CelShaderGenerator>(scene_manager_);
    cel_ready_ = cel_generator_->initialise();
    race_meshes_ = std::make_unique<AnthropomorphicMeshBuilder>(scene_manager_);

    build_camera();
    build_grid();
    build_combat_batches();
    telegraphs_ = std::make_unique<TelegraphRenderer>(scene_manager_);

    // The local player's cel-shaded view is built on the first
    // set_local_player call, once the race is known; the node exists now so
    // the position can be set before the body exists.
    local_player_.node = scene_manager_->getRootSceneNode()->createChildSceneNode("haven/local_player");
    local_player_.node->setPosition(0.0f, 0.0f, 0.0f);
}

void HavenRenderer::build_camera()
{
    camera_ = scene_manager_->createCamera("haven/camera");
    camera_->setProjectionType(Ogre::PT_ORTHOGRAPHIC);
    camera_->setOrthoWindow(kViewportWidth, kViewportHeight);
    camera_->setNearClipDistance(0.1f);
    camera_->setFarClipDistance(100.0f);

    camera_node_ = scene_manager_->getRootSceneNode()->createChildSceneNode("haven/camera_node");
    camera_node_->setPosition(0.0f, 0.0f, kCameraZ);
    camera_node_->setOrientation(Ogre::Quaternion::IDENTITY);
    camera_node_->attachObject(camera_);

    Ogre::Viewport* viewport = render_window()->addViewport(camera_);
    viewport->setBackgroundColour(colour(0.05f, 0.05f, 0.07f));
}

void HavenRenderer::build_grid()
{
    // createManualObject() does not attach the object to the scene graph, so it
    // needs an explicit node before it will be rendered.
    grid_ = scene_manager_->createManualObject("haven/grid");
    grid_->begin("haven/grid", Ogre::RenderOperation::OT_LINE_LIST, kResourceGroup);

    const float half = kArenaHalfSize;
    const float step = kGridCell;
    for (float x = -half; x <= half + 0.001f; x += step)
    {
        grid_->position(x, -half, -0.05f);
        grid_->position(x, half, -0.05f);
    }
    for (float y = -half; y <= half + 0.001f; y += step)
    {
        grid_->position(-half, y, -0.05f);
        grid_->position(half, y, -0.05f);
    }
    grid_->end();
    scene_manager_->getRootSceneNode()->createChildSceneNode("haven/grid_node")->attachObject(grid_);

    // Arena boundary walls, drawn slightly in front of the grid.
    Ogre::ManualObject* walls = scene_manager_->createManualObject("haven/arena_walls");
    walls->begin("haven/arena_wall", Ogre::RenderOperation::OT_LINE_LIST, kResourceGroup);
    walls->position(-half, -half, 0.02f);
    walls->position(half, -half, 0.02f);
    walls->position(half, -half, 0.02f);
    walls->position(half, half, 0.02f);
    walls->position(half, half, 0.02f);
    walls->position(-half, half, 0.02f);
    walls->position(-half, half, 0.02f);
    walls->position(-half, -half, 0.02f);
    walls->end();
    scene_manager_->getRootSceneNode()->createChildSceneNode("haven/walls_node")->attachObject(walls);
}

/// Uploads into a Manual Object through the same immediate-mode sink the
/// geometry builders write to, so the batch path and the tested builders are
/// one code path. Vertices carry no colour: the material owns it.
///
/// For the pools whose material is the cel-shaded toon shader (enemy bodies,
/// projectiles and gems) the sink bakes a fake radial normal per vertex,
/// domed from the item's position, which is what the toon shader quantises
/// into bands. The flat pools keep the plain (0,0,1) normal their materials
/// have always used, so their rendering is unchanged.
class ManualPrimitiveSink : public PrimitiveSink {
public:
    explicit ManualPrimitiveSink(Ogre::ManualObject& object) : object_(object) {}

    uint32_t base_index() const override { return vertex_count_; }

    void vertex(float x, float y, float z) override
    {
        object_.position(x, y, z);
        if (toon_)
        {
            const float dx = x - origin_.x;
            const float dy = y - origin_.y;
            const float nz = radius_ * 0.9f;
            const float inv = 1.0f / std::sqrt(dx * dx + dy * dy + nz * nz);
            object_.normal(dx * inv, dy * inv, nz * inv);
        }
        else
        {
            object_.normal(0.0f, 0.0f, 1.0f);
        }
        ++vertex_count_;
    }

    void index(uint32_t index) override { object_.index(index); }

    void reset() { vertex_count_ = 0; }

    /// True for the cel-shaded pools, false for the flat ones.
    void set_toon(bool toon) { toon_ = toon; }

    /// The shape's centre and radius, set per item, so each archetype, gem
    /// and projectile domes the right way.
    void set_shape(Vec2 origin, float radius)
    {
        origin_ = origin;
        radius_ = radius;
    }

private:
    Ogre::ManualObject& object_;
    uint32_t vertex_count_ = 0;
    bool toon_ = false;
    Vec2 origin_{0.0f, 0.0f};
    float radius_ = 1.0f;
};

/// Emits the geometry for one entity into one pool.
void append_to_pool(ColourPool pool, PrimitiveSink& sink, const EntityDrawItem& item)
{
    if (pool == ColourPool::EnemyBody)
        append_enemy_body(sink, item);
    else if (pool == ColourPool::Gem)
        append_gem(sink, item);
    else
        append_projectile(sink, item);
}

/// Segments in an elite outline ring: enough to read as a circle at the
/// 80 x 45 viewport without costing a second of vertex uploads.
constexpr uint32_t kOutlineSegments = 16;

/// One elite outline ring for modifier `bit`: a filled circle slightly larger
/// than the body, drawn just under it (and just under any inner ring), so the
/// body and the inner rings cover the middle and only the coloured annulus
/// shows. One concentric ring per set modifier bit reads as a multi-colour
/// outline around the archetype's own body colour.
void append_elite_ring(PrimitiveSink& sink, const EntityDrawItem& item, uint8_t bit)
{
    const float body_radius = enemy_radius(item.style);
    const float ring_radius = body_radius + 0.10f + 0.14f * static_cast<float>(bit);
    const float z = kZEnemy - 0.005f - 0.002f * static_cast<float>(bit);
    append_circle(sink, item.pos, ring_radius, z, kOutlineSegments);
}

/// The boss dash warning: a red wireframe rectangle spanning the dash path
/// ahead of the boss along its facing. DESIGN.md section 4 specifies a 1.0 s
/// red wireframe warning box before the 22.0 u/s dash.
void append_boss_telegraph(PrimitiveSink& sink, const EntityDrawItem& item)
{
    const float boss_radius = enemy_radius(item.style);
    const Vec2 centre{item.pos.x + std::cos(item.angle) * kBossDashUnits * 0.5f,
                       item.pos.y + std::sin(item.angle) * kBossDashUnits * 0.5f};
    const Vec2 half_extents{kBossDashUnits * 0.5f, boss_radius + 0.6f};
    append_wireframe_rect(sink, centre, half_extents, item.angle, kZProjectile + 0.005f, 0.18f);
}

/// The bar pool is drawn in two passes: every enemy's dark background first,
/// then each fill in its own colour slot. Both are quads at the same depth, so
/// the background has to be a separate section to avoid z-fighting.
void append_bar_background(PrimitiveSink& sink, const EntityDrawItem& item)
{
    append_enemy_bar_background(sink, item);
}

/// A slot key as a single array index.
size_t HavenRenderer::slot_index(const ColourSlot& slot)
{
    return static_cast<size_t>(slot.pool) * kMaxSlotsPerPool + slot.slot;
}

HavenRenderer::ColourBatch& HavenRenderer::batch_for(const ColourSlot& slot)
{
    return batches_[slot_index(slot)];
}

/// Worst-case vertex and index counts for one slot of a pool: every entity the
/// protocol allows could land in the same colour at once.
void HavenRenderer::slot_capacity(ColourPool pool, uint32_t& verts, uint32_t& indices)
{
    switch (pool)
    {
        case ColourPool::EnemyBody:
            verts = kMaxEnemies * kVertsPerEnemyBody;
            indices = kMaxEnemies * kIndicesPerEnemyBody;
            break;
        case ColourPool::EnemyBar:
            verts = kMaxEnemies * kVertsPerEnemyBar;
            indices = kMaxEnemies * kIndicesPerEnemyBar;
            break;
        case ColourPool::Projectile:
            verts = kMaxProjectiles * kVertsPerProjectile;
            indices = kMaxProjectiles * kIndicesPerProjectile;
            break;
        case ColourPool::Gem:
            verts = kMaxGems * kVertsPerGem;
            indices = kMaxGems * kIndicesPerGem;
            break;
        case ColourPool::EliteOutline:
            verts = kMaxEnemies * kVertsPerEliteRing;
            indices = kMaxEnemies * kIndicesPerEliteRing;
            break;
        case ColourPool::BossTelegraph:
            verts = kMaxBossesDrawn * kVertsPerTelegraph;
            indices = kMaxBossesDrawn * kIndicesPerTelegraph;
            break;
    }
}

Ogre::ManualObject* HavenRenderer::pool_object(ColourPool pool)
{
    switch (pool)
    {
        case ColourPool::EnemyBody: return enemy_bodies_;
        case ColourPool::EnemyBar: return enemy_bars_;
        case ColourPool::Projectile: return projectiles_;
        case ColourPool::Gem: return gem_bodies_;
        case ColourPool::EliteOutline: return elite_outlines_;
        case ColourPool::BossTelegraph: return boss_telegraph_;
    }
    return nullptr;
}

void HavenRenderer::create_slot(const ColourSlot& slot)
{
    Ogre::ManualObject* object = pool_object(slot.pool);
    if (object == nullptr) return;

    const std::string name = "haven/slot_" + std::to_string(static_cast<int>(slot.pool)) + "_" +
                             std::to_string(static_cast<int>(slot.slot));

    // The enemy bodies, projectiles and gems get the cel-shaded toon
    // material; the bar, elite-outline and telegraph pools are arena-floor
    // and HUD-style elements and stay flat. When the shaders failed to
    // compile, every pool falls back to a flat material.
    const bool toon = cel_ready_ && (slot.pool == ColourPool::EnemyBody || slot.pool == ColourPool::Projectile ||
                                     slot.pool == ColourPool::Gem);
    Ogre::MaterialPtr material;
    if (toon)
        material = cel_generator_->batch_material(name, colour(slot_colour(slot)));
    else
        material = meshes_->flat_material(name, colour(slot_colour(slot)));

    uint32_t verts = 0;
    uint32_t indices = 0;
    slot_capacity(slot.pool, verts, indices);
    object->estimateVertexCount(verts);
    object->estimateIndexCount(indices);
    object->begin(name, Ogre::RenderOperation::OT_TRIANGLE_LIST, kResourceGroup);

    // One triangle so the section exists before the first real frame: a Manual
    // Object with no vertices discards its section, and beginUpdate() on a
    // discarded section is undefined. The indices are just as necessary:
    // end() dereferences the index buffer of an updating section, so a section
    // created without one crashes the first time real geometry goes in.
    for (uint32_t i = 0; i < 3; ++i)
    {
        object->position(0.0f, 0.0f, 0.0f);
        object->normal(0.0f, 0.0f, 1.0f);
        object->index(i);
    }
    object->end();

    ColourBatch& batch = batch_for(slot);
    batch.object = object;
    batch.section = object->getNumSections() - 1;
    batch.capacity = verts;
    batch.created = true;
    object->setBoundingBox(Ogre::AxisAlignedBox(-kArenaHalfSize, -kArenaHalfSize, -1.0f, kArenaHalfSize,
                                                kArenaHalfSize, 1.0f));
}

void HavenRenderer::build_combat_batches()
{
    // One Manual Object per draw layer, so an entity's z never has to change.
    // Sections are added per colour on first use; the objects themselves are
    // created here so the render loop never has to build scene objects.
    struct Pool {
        const char* name;
        Ogre::ManualObject** slot;
    };
    const Pool pools[] = {
        {"haven/enemy_bodies", &enemy_bodies_},
        {"haven/enemy_bars", &enemy_bars_},
        {"haven/projectiles", &projectiles_},
        {"haven/gem_bodies", &gem_bodies_},
        {"haven/elite_outlines", &elite_outlines_},
        {"haven/boss_telegraph", &boss_telegraph_},
    };

    for (const Pool& pool : pools)
    {
        Ogre::ManualObject* object = scene_manager_->createManualObject(pool.name);
        object->setDynamic(true);
        object->setVisible(false);
        scene_manager_->getRootSceneNode()->createChildSceneNode(Ogre::String(pool.name) + "_node")
            ->attachObject(object);
        *pool.slot = object;
    }
}

void HavenRenderer::render_entities(const std::vector<EntityDrawItem>& items, uint32_t run_time_ms)
{
    if (enemy_bodies_ == nullptr || enemy_bars_ == nullptr || projectiles_ == nullptr) return;

    for (size_t i = 0; i < kSlotItemCount; ++i) slot_items_[i].clear();

    // Group the entities by the colour each is drawn in. An enemy appears
    // several times over: its body, the dark background of its HP bar, the
    // fill, and — when it is elite — one outline ring per set modifier bit.
    // A boss in the dash-warning window also contributes its telegraph box.
    for (const EntityDrawItem& item : items)
    {
        if (item.kind == EntityKind::Enemy)
        {
            const ColourSlot body = slot_for_body(item);
            const ColourSlot background = ColourSlot{ColourPool::EnemyBar, 0};
            const ColourSlot fill = slot_for_bar(item);
            slot_items_[slot_index(body)].push_back(&item);
            slot_items_[slot_index(background)].push_back(&item);
            slot_items_[slot_index(fill)].push_back(&item);

            for (uint8_t bit = 0; bit < kEliteOutlineSlots; ++bit)
            {
                if ((item.elite_mask & (1u << bit)) == 0) continue;
                const ColourSlot ring = slot_for_elite_outline(bit);
                slot_items_[slot_index(ring)].push_back(&item);
            }

            if (item.style == static_cast<uint8_t>(Archetype::Boss) && boss_telegraph_active(run_time_ms))
            {
                const ColourSlot telegraph = slot_for_boss_telegraph();
                slot_items_[slot_index(telegraph)].push_back(&item);
            }
        }
        else if (item.kind == EntityKind::Projectile)
        {
            const ColourSlot proj = slot_for_projectile(item);
            slot_items_[slot_index(proj)].push_back(&item);
        }
        else if (item.kind == EntityKind::Gem)
        {
            const ColourSlot gem = slot_for_gem(item);
            slot_items_[slot_index(gem)].push_back(&item);
        }
    }

    // The geometry is rebuilt from scratch every frame, so the batches are
    // always drawn: nothing is culled against last frame's data. That means
    // their bounding volume has to be set every frame too, and it has to be
    // centred on the camera rather than on the origin, because ManualObject
    // culls against a sphere built from the volume's centre.
    if (camera_node_ != nullptr)
    {
        const Ogre::Vector3 cam = camera_node_->getPosition();
        for (uint8_t pool = 0; pool < kPoolCount; ++pool)
        {
            Ogre::ManualObject* object = pool_object(static_cast<ColourPool>(pool));
            if (object != nullptr)
            {
                object->setBoundingBox(Ogre::AxisAlignedBox(cam.x - kArenaHalfSize, cam.y - kArenaHalfSize,
                                                            -1.0f, cam.x + kArenaHalfSize,
                                                            cam.y + kArenaHalfSize, 1.0f));
            }
        }
    }

    for (uint8_t pool = 0; pool < kPoolCount; ++pool)
    {
        const ColourPool colour_pool = static_cast<ColourPool>(pool);
        Ogre::ManualObject* object = pool_object(colour_pool);
        bool any = false;

        for (uint8_t slot = 0; slot < kMaxSlotsPerPool; ++slot)
        {
            const ColourSlot key{colour_pool, slot};
            ColourBatch& batch = batches_[pool * kMaxSlotsPerPool + slot];
            if (!batch.created)
            {
                // Only create a slot something actually uses; an unused colour
                // costs no material and no buffer.
                if (slot_items_[slot_index(key)].empty()) continue;
                create_slot(key);
            }
            if (!batch.created) continue;

            object->beginUpdate(batch.section);
            ManualPrimitiveSink sink(*object);
            const bool toon = cel_ready_ && (colour_pool == ColourPool::EnemyBody ||
                                             colour_pool == ColourPool::Projectile || colour_pool == ColourPool::Gem);
            sink.set_toon(toon);
            if (colour_pool == ColourPool::EliteOutline)
            {
                for (const EntityDrawItem* item : slot_items_[slot_index(key)])
                {
                    sink.set_shape(item->pos, enemy_radius(item->style));
                    append_elite_ring(sink, *item, slot);
                }
            }
            else if (colour_pool == ColourPool::BossTelegraph)
            {
                for (const EntityDrawItem* item : slot_items_[slot_index(key)])
                {
                    sink.set_shape(item->pos, enemy_radius(item->style));
                    append_boss_telegraph(sink, *item);
                }
            }
            else if (colour_pool == ColourPool::EnemyBar && slot == 0)
            {
                for (const EntityDrawItem* item : slot_items_[slot_index(key)])
                {
                    sink.set_shape(item->pos, 1.0f);
                    append_bar_background(sink, *item);
                }
            }
            else
            {
                for (const EntityDrawItem* item : slot_items_[slot_index(key)])
                {
                    float radius = 1.0f;
                    if (colour_pool == ColourPool::EnemyBody) radius = enemy_radius(item->style);
                    else if (colour_pool == ColourPool::Gem) radius = gem_radius(item->style);
                    else if (colour_pool == ColourPool::Projectile) radius = weapon_visual(item->style).radius;
                    sink.set_shape(item->pos, radius);
                    append_to_pool(colour_pool, sink, *item);
                }
            }
            object->end();
            any = true;
        }

        // Every slot is rewritten each frame, even the empty ones: a section
        // that is left alone keeps drawing last frame's geometry. An empty
        // section draws nothing, and the object as a whole is hidden when no
        // slot in it produced anything this frame.
        object->setVisible(any);
    }

    // The windup warnings under the enemies: one pooled shape per enemy whose
    // telegraph progress is above zero.
    if (telegraphs_ != nullptr) telegraphs_->render(items);
}

void HavenRenderer::attach_input(InputManager* input)
{
    input_ = input;
    // The renderer is the listener: OgreBites delivers the window's keyboard
    // events here and each is forwarded into the manager, which merges it with
    // the gamepad and keeps the wrapped InputState.
    addInputListener(this);
}

bool HavenRenderer::keyPressed(const OgreBites::KeyboardEvent& evt)
{
    if (input_ != nullptr) input_->keyPressed(evt.keysym.sym);
    return false;
}

bool HavenRenderer::keyReleased(const OgreBites::KeyboardEvent& evt)
{
    if (input_ != nullptr) input_->keyReleased(evt.keysym.sym);
    return false;
}

void HavenRenderer::run(const std::function<void(float)>& tick)
{
    tick_ = tick;
    mRoot->startRendering();
}

bool HavenRenderer::frameRenderingQueued(const Ogre::FrameEvent& evt)
{
    if (input_ != nullptr && input_->quit_requested())
    {
        input_->consume_quit();
        quit_ = true;
        mRoot->queueEndRendering();
        return true;
    }

    if (tick_) tick_(evt.timeSinceLastFrame);
    return true;
}

void HavenRenderer::shutdown()
{
    // The telegraph shapes are scene objects; they must go before closeApp()
    // tears the scene manager down, the same way hud.shutdown() goes first.
    if (telegraphs_ != nullptr)
    {
        telegraphs_->shutdown();
        telegraphs_.reset();
    }
}

void HavenRenderer::follow_camera(const Vec2& pos)
{
    if (camera_node_ != nullptr) camera_node_->setPosition(pos.x, pos.y, kCameraZ);
}

void HavenRenderer::set_local_player(const Vec2& pos, float angle, uint8_t race_id)
{
    if (local_player_.node == nullptr) return;
    ensure_player(local_player_, "haven/local_player", race_id);
    position_player(local_player_, pos, angle);
    update_cape(local_player_, pos);
}

void HavenRenderer::set_remote_player(uint8_t player_id, const Vec2& pos, float angle, uint8_t race_id)
{
    auto it = remote_players_.find(player_id);
    if (it == remote_players_.end())
    {
        PlayerView view;
        view.node =
            scene_manager_->getRootSceneNode()->createChildSceneNode("haven/remote_player_node_" + std::to_string(player_id));
        it = remote_players_.emplace(player_id, view).first;
    }
    ensure_player(it->second, "haven/remote_player_" + std::to_string(player_id), race_id);
    position_player(it->second, pos, angle);
    update_cape(it->second, pos);
}

void HavenRenderer::remove_player(uint8_t player_id)
{
    auto it = remote_players_.find(player_id);
    if (it == remote_players_.end()) return;
    PlayerView& view = it->second;
    if (view.body != nullptr)
    {
        view.node->detachObject(view.body);
        scene_manager_->destroyEntity(view.body);
    }
    if (view.cape != nullptr)
    {
        view.cape_node->detachObject(view.cape);
        scene_manager_->destroyEntity(view.cape);
    }
    if (view.cape_node != nullptr)
    {
        view.node->removeChild(view.cape_node);
        scene_manager_->destroySceneNode(view.cape_node);
    }
    scene_manager_->destroySceneNode(view.node);
    remote_players_.erase(it);
}

void HavenRenderer::clear_remote_players()
{
    for (auto& entry : remote_players_)
    {
        PlayerView& view = entry.second;
        if (view.body != nullptr)
        {
            view.node->detachObject(view.body);
            scene_manager_->destroyEntity(view.body);
        }
        if (view.cape != nullptr)
        {
            view.cape_node->detachObject(view.cape);
            scene_manager_->destroyEntity(view.cape);
        }
        if (view.cape_node != nullptr)
        {
            view.node->removeChild(view.cape_node);
            scene_manager_->destroySceneNode(view.cape_node);
        }
        scene_manager_->destroySceneNode(view.node);
    }
    remote_players_.clear();
}

Ogre::MaterialPtr HavenRenderer::body_material()
{
    if (body_material_.isNull())
    {
        if (cel_ready_)
            body_material_ = cel_generator_->cel_material("haven/player_body_mat", Ogre::ColourValue::White, kInkNormal);
        else
            body_material_ = meshes_->flat_material("haven/player_body_flat", colour(0.30f, 0.85f, 0.45f));
    }
    return body_material_;
}

Ogre::MaterialPtr HavenRenderer::cape_material(uint8_t race_id)
{
    auto it = cape_materials_.find(race_id);
    if (it != cape_materials_.end()) return it->second;
    const Ogre::ColourValue accent = AnthropomorphicMeshBuilder::race_accent_colour(race_id);
    Ogre::MaterialPtr material;
    if (cel_ready_)
        material = cel_generator_->cel_material("haven/player_cape_mat_" + std::to_string(race_id), accent, kInkNormal);
    else
        material = meshes_->flat_material("haven/player_cape_flat_" + std::to_string(race_id), accent);
    cape_materials_.emplace(race_id, material);
    return material;
}

HavenRenderer::PlayerView& HavenRenderer::ensure_player(PlayerView& view, const std::string& name, uint8_t race_id)
{
    if (view.body != nullptr && view.race_id == race_id) return view;

    // Tear down a view that was built for a different race.
    if (view.body != nullptr)
    {
        view.node->detachObject(view.body);
        scene_manager_->destroyEntity(view.body);
        view.body = nullptr;
    }
    if (view.cape != nullptr)
    {
        view.cape_node->detachObject(view.cape);
        scene_manager_->destroyEntity(view.cape);
        view.cape = nullptr;
    }
    if (view.cape_node != nullptr)
    {
        view.node->removeChild(view.cape_node);
        scene_manager_->destroySceneNode(view.cape_node);
        view.cape_node = nullptr;
    }

    Ogre::MeshPtr body_mesh = cel_ready_ ? race_meshes_->body_mesh_for_race(race_id)
                                          : Ogre::MeshManager::getSingleton().getByName("haven/player_square", kResourceGroup);
    view.body = scene_manager_->createEntity(name + "_body", body_mesh->getName(), kResourceGroup);
    view.body->setMaterialName(body_material()->getName(), kResourceGroup);
    view.node->attachObject(view.body);

    if (cel_ready_)
    {
        Ogre::MeshPtr cape_mesh = race_meshes_->cape_mesh_for_race(race_id);
        view.cape_node = view.node->createChildSceneNode(name + "_cape_node");
        view.cape = scene_manager_->createEntity(name + "_cape", cape_mesh->getName(), kResourceGroup);
        view.cape->setMaterialName(cape_material(race_id)->getName(), kResourceGroup);
        view.cape_node->attachObject(view.cape);
    }

    view.race_id = race_id;
    return view;
}

void HavenRenderer::position_player(PlayerView& view, const Vec2& pos, float angle)
{
    view.facing = angle;
    view.node->setPosition(pos.x, pos.y, 0.0f);
    view.node->setOrientation(Ogre::Quaternion(Ogre::Radian(angle), Ogre::Vector3::UNIT_Z));
}

void HavenRenderer::update_cape(PlayerView& view, const Vec2& pos)
{
    if (view.cape_node == nullptr) return;
    // The cape trails behind the velocity vector: its world direction is the
    // velocity direction plus pi, so its local rotation relative to the
    // facing node is (velocity angle - facing). When the player is stationary
    // the last angle is kept, so the cape does not snap to a default.
    if (view.pos_valid)
    {
        const Vec2 delta = pos - view.last_pos;
        if (length(delta) > 0.03f)
        {
            const float velocity_angle = std::atan2(delta.y, delta.x);
            view.cape_angle = velocity_angle - view.facing;
        }
    }
    view.last_pos = pos;
    view.pos_valid = true;
    view.cape_node->setOrientation(Ogre::Quaternion(Ogre::Radian(view.cape_angle), Ogre::Vector3::UNIT_Z));
}

void HavenRenderer::capture_framebuffer(const std::string& path) const
{
    if (render_window() != nullptr) render_window()->writeContentsToFile(path);
}

}  // namespace haven
