#include "renderer.h"

#include <OgreEntity.h>
#include <OgreMaterialManager.h>
#include <OgreMeshManager.h>
#include <OgreRenderOperation.h>
#include <OgreResourceGroupManager.h>
#include <OgreRoot.h>
#include <OgreViewport.h>

#include <cmath>
#include <cstdio>

namespace haven {

namespace {

constexpr const char* kResourceGroup = "ShapedHaven";

/// DESIGN.md section 1: the orthographic viewport covers 80 x 45 units.
constexpr float kViewportWidth = 80.0f;
constexpr float kViewportHeight = 45.0f;

constexpr float kGridCell = 5.0f;

Ogre::ColourValue colour(float r, float g, float b)
{
    return Ogre::ColourValue(r, g, b);
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

    build_camera();
    build_grid();

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

    // The camera tracks the local player so the 80x45 viewport stays centred
    // on the action inside the 2000x2000 arena.
    camera_node_->setPosition(local_player_node_->getPosition().x, local_player_node_->getPosition().y, 10.0f);

    if (tick_) tick_(evt.timeSinceLastFrame);
    return true;
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
