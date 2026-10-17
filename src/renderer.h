#pragma once

#include "game_math.h"
#include "entity_frame.h"
#include "input_state.h"
#include "mesh_factory.h"

#include <vector>

#include <OgreApplicationContext.h>
#include <OgreCamera.h>
#include <OgreFrameListener.h>
#include <OgreGL3PlusPlugin.h>
#include <OgreInput.h>
#include <OgreManualObject.h>
#include <OgreRenderWindow.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreSTBICodec.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace haven {

/// The Ogre shell: window, orthographic camera, procedural meshes, the arena
/// grid and the entity views that the game loop moves around.
///
/// OgreBites::ApplicationContext is used for the window, the render loop and
/// input plumbing. It is subclassed rather than composed because Bites owns the
/// SDL window and only dispatches input events to listeners registered against
/// a window it created itself.
class HavenRenderer : public OgreBites::ApplicationContext, public OgreBites::InputListener {
public:
    HavenRenderer();

    /// Creates the root, the window, the scene and the meshes. Returns false
    /// if Ogre could not be brought up.
    bool initialise();

    /// Runs the render loop until the window closes or `quit` is set. `tick` is
    /// invoked once per rendered frame with the frame delta in seconds.
    void run(const std::function<void(float)>& tick);

    /// Registers the keyboard state that produces movement input.
    void attach_input(InputState* input);

    bool keyPressed(const OgreBites::KeyboardEvent& evt) override;
    bool keyReleased(const OgreBites::KeyboardEvent& evt) override;

    /// Renders the current framebuffer, used by the manual verification run.
    void capture_framebuffer(const std::string& path) const;

    Ogre::RenderWindow* render_window() const { return mWindows.empty() ? nullptr : mWindows[0].render; }
    Ogre::SceneManager* scene_manager() const { return scene_manager_; }
    Ogre::SceneNode* camera_node() const { return camera_node_; }
    MeshFactory* mesh_factory() const { return meshes_.get(); }

    /// Local player square, driven by the prediction state.
    void set_local_player(const Vec2& pos);

    /// Keeps the 80x45 viewport centred on the local player, so the action
    /// stays in view inside the 2000x2000 arena. Called by the game loop after
    /// it advances the player, so tracking does not lag a frame behind.
    void follow_camera(const Vec2& pos);

    /// Remote players, driven by the interpolation buffer.
    void set_remote_player(uint8_t player_id, const Vec2& pos);
    void remove_player(uint8_t player_id);
    void clear_remote_players();

    /// Enemies and projectiles, rebuilt from scratch every frame out of the
    /// interpolated draw items the session hands over. One Manual Object per
    /// draw layer holds the whole batch, so a thousand enemies cost one draw
    /// call each instead of one entity each. `run_time_ms` is the newest
    /// snapshot's run clock, which drives the boss dash warning box.
    void render_entities(const std::vector<EntityDrawItem>& items, uint32_t run_time_ms);

private:
    /// One colour slot's worth of geometry inside its pool's Manual Object.
    struct ColourBatch {
        Ogre::ManualObject* object = nullptr;
        uint32_t section = 0;
        uint32_t capacity = 0;
        bool created = false;
    };

    void createRoot();
    bool oneTimeConfig();
    void locateResources();
    void loadResources();
    void setup();

    void build_scene();
    void build_grid();
    void build_camera();
    void build_combat_batches();

    size_t slot_index(const ColourSlot& slot);
    ColourBatch& batch_for(const ColourSlot& slot);
    void slot_capacity(ColourPool pool, uint32_t& verts, uint32_t& indices);

    /// The Manual Object that holds one draw layer's sections.
    Ogre::ManualObject* pool_object(ColourPool pool);

    /// Adds a section (and its material) for a colour slot on first use.
    void create_slot(const ColourSlot& slot);

    /// The orthographic viewport is a fixed 80 x 45 units, so the window is
    /// forced to the matching 16:9 aspect ratio to avoid stretching.
    OgreBites::NativeWindowPair createWindow(const Ogre::String& name, uint32_t w, uint32_t h,
                                             Ogre::NameValuePairList miscParams) override;

    bool frameRenderingQueued(const Ogre::FrameEvent& evt) override;

    Ogre::SceneManager* scene_manager_ = nullptr;
    Ogre::Camera* camera_ = nullptr;
    Ogre::SceneNode* camera_node_ = nullptr;
    Ogre::SceneNode* local_player_node_ = nullptr;
    Ogre::ManualObject* grid_ = nullptr;
    std::unique_ptr<MeshFactory> meshes_;
    InputState* input_ = nullptr;

    /// One Manual Object per draw layer, each holding one section per colour
    /// slot actually in use. See render_entities().
    Ogre::ManualObject* enemy_bodies_ = nullptr;
    Ogre::ManualObject* enemy_bars_ = nullptr;
    Ogre::ManualObject* projectiles_ = nullptr;
    Ogre::ManualObject* gem_bodies_ = nullptr;
    Ogre::ManualObject* elite_outlines_ = nullptr;
    Ogre::ManualObject* boss_telegraph_ = nullptr;

    /// Scratch storage grouping the frame's entities by colour slot, and the
    /// slots that exist. Both are indexed pool * kMaxSlotsPerPool + slot.
    static constexpr size_t kSlotItemCount = kPoolCount * kMaxSlotsPerPool;
    std::vector<const EntityDrawItem*> slot_items_[kSlotItemCount];
    ColourBatch batches_[kSlotItemCount];

    std::function<void(float)> tick_;
    bool quit_ = false;

    std::unordered_map<uint8_t, Ogre::SceneNode*> remote_players_;
};

}  // namespace haven
