#include "renderer.h"

#include <OgreEntity.h>
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
#include <memory>

namespace haven {

namespace {

constexpr const char* kResourceGroup = "ShapedHaven";

/// DESIGN.md section 1: the orthographic viewport covers 80 x 45 units.
constexpr float kViewportWidth = 80.0f;
constexpr float kViewportHeight = 45.0f;

constexpr float kGridCell = 5.0f;

/// How many enemies and projectiles the per-frame batches are sized for. These
/// are the DESIGN.md section 2 pool caps, so a full pool never makes the
/// batches grow mid-frame; if a server ever sent more than the cap, the
/// excess would be dropped rather than corrupting the buffer.
constexpr uint32_t kBatchEnemies = 4096;
constexpr uint32_t kBatchProjectiles = 2048;

/// Vertices per enemy: a 16 segment circle fan is the worst case, and two
/// quads (background plus fill) make up the HP bar.
constexpr uint32_t kVertsPerEnemyBody = 17;
constexpr uint32_t kVertsPerEnemyBar = 8;
constexpr uint32_t kIndicesPerEnemyBody = 48;
constexpr uint32_t kIndicesPerEnemyBar = 12;

/// Vertices per projectile: an arc at 24 segments is the worst case.
constexpr uint32_t kVertsPerProjectile = 25;
constexpr uint32_t kIndicesPerProjectile = 72;

Ogre::ColourValue colour(float r, float g, float b)
{
    return Ogre::ColourValue(r, g, b);
}

Ogre::ColourValue colour(const Rgba& rgba)
{
    return Ogre::ColourValue(rgba.r, rgba.g, rgba.b, rgba.a);
}

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
    meshes_->flat_material("haven/player", colour(0.30f, 0.85f, 0.45f));
    meshes_->flat_material("haven/remote_player", colour(0.35f, 0.55f, 0.95f));
    meshes_->flat_material("haven/grid", colour(0.18f, 0.20f, 0.26f));
    meshes_->flat_material("haven/arena_wall", colour(0.55f, 0.35f, 0.85f));
    // White, unlit, and tracking the per-vertex colour: the combat batches put
    // an entity's whole colour into the vertex, so one material serves every
    // enemy and every projectile.
    Ogre::MaterialPtr batch = meshes_->flat_material("haven/batch", colour(1.0f, 1.0f, 1.0f));
    batch->getTechnique(0)->getPass(0)->setVertexColourTracking(Ogre::TVC_DIFFUSE);

    build_camera();
    build_grid();
    build_combat_batches();

    Ogre::Entity* player = scene_manager_->createEntity("haven/player_square");
    player->setMaterialName("haven/player", kResourceGroup);
    local_player_node_ = scene_manager_->getRootSceneNode()->createChildSceneNode("haven/local_player");
    local_player_node_->attachObject(player);
    local_player_node_->setPosition(0.0f, 0.0f, 0.0f);
}

void HavenRenderer::build_camera()
{
    camera_ = scene_manager_->createCamera("haven/camera");
    camera_->setProjectionType(Ogre::PT_ORTHOGRAPHIC);
    camera_->setOrthoWindow(kViewportWidth, kViewportHeight);
    camera_->setNearClipDistance(0.1f);
    camera_->setFarClipDistance(100.0f);

    camera_node_ = scene_manager_->getRootSceneNode()->createChildSceneNode("haven/camera_node");
    camera_node_->setPosition(0.0f, 0.0f, 10.0f);
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
/// one code path.
class ManualPrimitiveSink : public PrimitiveSink {
public:
    explicit ManualPrimitiveSink(Ogre::ManualObject& object) : object_(object) {}

    uint32_t base_index() const override { return vertex_count_; }

    void vertex(float x, float y, float z, const Rgba& rgba) override
    {
        object_.position(x, y, z);
        object_.normal(0.0f, 0.0f, 1.0f);
        object_.colour(rgba.r, rgba.g, rgba.b, rgba.a);
        ++vertex_count_;
    }

    void index(uint32_t index) override { object_.index(index); }

    void reset() { vertex_count_ = 0; }

private:
    Ogre::ManualObject& object_;
    uint32_t vertex_count_ = 0;
};

void HavenRenderer::build_combat_batches()
{
    struct Batch {
        const char* name;
        Ogre::ManualObject** slot;
        uint32_t verts;
        uint32_t indices;
    };
    // One material per batch: the colour lives in the vertex, so a single
    // unlit material with vertex colour tracking serves every entity in it.
    const Batch batches[] = {
        {"haven/enemy_bodies", &enemy_bodies_, kBatchEnemies * kVertsPerEnemyBody,
         kBatchEnemies * kIndicesPerEnemyBody},
        {"haven/enemy_bars", &enemy_bars_, kBatchEnemies * kVertsPerEnemyBar,
         kBatchEnemies * kIndicesPerEnemyBar},
        {"haven/projectiles", &projectiles_, kBatchProjectiles * kVertsPerProjectile,
         kBatchProjectiles * kIndicesPerProjectile},
    };

    for (const Batch& batch : batches)
    {
        Ogre::ManualObject* object = scene_manager_->createManualObject(batch.name);
        object->setDynamic(true);
        object->estimateVertexCount(batch.verts);
        object->estimateIndexCount(batch.indices);
        object->begin("haven/batch", Ogre::RenderOperation::OT_TRIANGLE_LIST, kResourceGroup);

        // One degenerate triangle so the section exists before the first real
        // frame: a Manual Object with no vertices discards its section, and
        // beginUpdate() on a discarded section is undefined.
        for (int i = 0; i < 3; ++i)
        {
            object->position(0.0f, 0.0f, 0.0f);
            object->normal(0.0f, 0.0f, 1.0f);
            object->colour(0.0f, 0.0f, 0.0f, 0.0f);
        }
        object->end();

        // The arena-sized box keeps the batch in frustum no matter where the
        // camera has followed the player; the geometry is rebuilt every frame
        // anyway, so there is nothing to cull against.
        object->setBoundingBox(Ogre::AxisAlignedBox(-kArenaHalfSize, -kArenaHalfSize, -1.0f, kArenaHalfSize,
                                                    kArenaHalfSize, 1.0f));
        object->setVisible(false);
        scene_manager_->getRootSceneNode()
            ->createChildSceneNode(Ogre::String(batch.name) + "_node")
            ->attachObject(object);

        *batch.slot = object;
    }
}

void HavenRenderer::render_entities(const std::vector<EntityDrawItem>& items)
{
    if (enemy_bodies_ == nullptr || enemy_bars_ == nullptr || projectiles_ == nullptr) return;

    size_t enemies = 0;
    size_t projectiles = 0;
    for (const EntityDrawItem& item : items)
    {
        if (item.kind == EntityKind::Enemy) ++enemies;
        else if (item.kind == EntityKind::Projectile) ++projectiles;
    }

    // Nothing to draw: hide the batches rather than updating them with an
    // empty frame.
    if (enemies == 0 && projectiles == 0)
    {
        enemy_bodies_->setVisible(false);
        enemy_bars_->setVisible(false);
        projectiles_->setVisible(false);
        return;
    }

    // Each batch is a fixed size, so the counts are clamped to what fits. The
    // protocol caps are higher than these batch sizes only for enemies, and a
    // 4096 enemy pool is what the batches are sized for.
    if (enemies > kBatchEnemies) enemies = kBatchEnemies;
    if (projectiles > kBatchProjectiles) projectiles = kBatchProjectiles;

    enemy_bodies_->beginUpdate(0);
    enemy_bars_->beginUpdate(0);
    projectiles_->beginUpdate(0);
    ManualPrimitiveSink bodies(*enemy_bodies_);
    ManualPrimitiveSink bars(*enemy_bars_);
    ManualPrimitiveSink projectile_sink(*projectiles_);

    for (const EntityDrawItem& item : items)
    {
        if (item.kind == EntityKind::Enemy)
        {
            if (enemies == 0) continue;
            --enemies;
            append_enemy(bodies, bars, item);
        }
        else if (item.kind == EntityKind::Projectile)
        {
            if (projectiles == 0) continue;
            --projectiles;
            append_projectile(projectile_sink, item);
        }
    }

    enemy_bodies_->end();
    enemy_bars_->end();
    projectiles_->end();
    enemy_bodies_->setVisible(true);
    enemy_bars_->setVisible(true);
    projectiles_->setVisible(true);
}

void HavenRenderer::attach_input(InputState* input)
{
    input_ = input;
    addInputListener(this);
}

bool HavenRenderer::keyPressed(const OgreBites::KeyboardEvent& evt)
{
    if (input_ != nullptr) input_->key_down(evt.keysym.sym);
    return false;
}

bool HavenRenderer::keyReleased(const OgreBites::KeyboardEvent& evt)
{
    if (input_ != nullptr) input_->key_up(evt.keysym.sym);
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

void HavenRenderer::follow_camera(const Vec2& pos)
{
    if (camera_node_ != nullptr) camera_node_->setPosition(pos.x, pos.y, 10.0f);
}

void HavenRenderer::set_local_player(const Vec2& pos)
{
    if (local_player_node_ != nullptr) local_player_node_->setPosition(pos.x, pos.y, 0.0f);
}

void HavenRenderer::set_remote_player(uint8_t player_id, const Vec2& pos)
{
    auto it = remote_players_.find(player_id);
    if (it == remote_players_.end())
    {
        Ogre::Entity* entity = scene_manager_->createEntity("haven/remote_player_" + std::to_string(player_id),
                                                            "haven/player_square", kResourceGroup);
        entity->setMaterialName("haven/remote_player", kResourceGroup);
        Ogre::SceneNode* node = scene_manager_->getRootSceneNode()->createChildSceneNode();
        node->attachObject(entity);
        it = remote_players_.emplace(player_id, node).first;
    }
    it->second->setPosition(pos.x, pos.y, -0.01f);
}

void HavenRenderer::remove_player(uint8_t player_id)
{
    auto it = remote_players_.find(player_id);
    if (it == remote_players_.end()) return;
    scene_manager_->destroySceneNode(it->second);
    remote_players_.erase(it);
}

void HavenRenderer::clear_remote_players()
{
    for (auto& entry : remote_players_)
        scene_manager_->destroySceneNode(entry.second);
    remote_players_.clear();
}

void HavenRenderer::capture_framebuffer(const std::string& path) const
{
    if (render_window() != nullptr) render_window()->writeContentsToFile(path);
}

}  // namespace haven
