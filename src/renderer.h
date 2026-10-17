#pragma once

#include "game_math.h"
#include "entity_frame.h"
#include "input/InputManager.h"
#include "mesh_factory.h"
#include "render/AnthropomorphicMeshBuilder.h"
#include "render/CelShaderGenerator.h"
#include "render/TelegraphRenderer.h"

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

    /// Registers the input manager the window's keyboard events feed. The
    /// renderer stays the OgreBites input listener and forwards each key event
    /// into the manager, which merges it with the gamepad.
    void attach_input(InputManager* input);

    bool keyPressed(const OgreBites::KeyboardEvent& evt) override;
    bool keyReleased(const OgreBites::KeyboardEvent& evt) override;

    /// Renders the current framebuffer, used by the manual verification run.
    void capture_framebuffer(const std::string& path) const;

    Ogre::RenderWindow* render_window() const { return mWindows.empty() ? nullptr : mWindows[0].render; }
    Ogre::SceneManager* scene_manager() const { return scene_manager_; }
    Ogre::SceneNode* camera_node() const { return camera_node_; }
    MeshFactory* mesh_factory() const { return meshes_.get(); }

    /// True when the cel-shading shaders compiled. When false the renderer
    /// draws the flat coloured-primitive fallback, so the game stays playable
    /// on a GPU that rejects the custom shaders.
    bool cel_shading_ready() const { return cel_ready_; }
    CelShaderGenerator* cel_generator() const { return cel_generator_.get(); }
    AnthropomorphicMeshBuilder* race_meshes() const { return race_meshes_.get(); }

    /// Local player, driven by the prediction state. `angle` is the facing in
    /// radians and `race_id` picks the anthropomorphic mesh; a changed race
    /// rebuilds the view. Called every predicted tick and every frame.
    void set_local_player(const Vec2& pos, float angle, uint8_t race_id);

    /// Keeps the 80x45 viewport centred on the local player, so the action
    /// stays in view inside the 2000x2000 arena. Called by the game loop after
    /// it advances the player, so tracking does not lag a frame behind.
    void follow_camera(const Vec2& pos);

    /// Remote players, driven by the interpolation buffer. `angle` is the
    /// decoded facing and `race_id` the race the player picked, so a remote
    /// shows the same cel-shaded silhouette the local player would.
    void set_remote_player(uint8_t player_id, const Vec2& pos, float angle, uint8_t race_id);
    void remove_player(uint8_t player_id);
    void clear_remote_players();

    /// Enemies and projectiles, rebuilt from scratch every frame out of the
    /// interpolated draw items the session hands over. One Manual Object per
    /// draw layer holds the whole batch, so a thousand enemies cost one draw
    /// call each instead of one entity each. `run_time_ms` is the newest
    /// snapshot's run clock, which drives the boss dash warning box.
    void render_entities(const std::vector<EntityDrawItem>& items, uint32_t run_time_ms);

    /// Destroys the telegraph shapes' scene objects while Ogre is still
    /// alive. Must be called before closeApp().
    void shutdown();

private:
    /// One cel-shaded character: the body mesh on a node that faces the
    /// movement direction, and the cape/scarf mesh on a child node that trails
    /// behind the velocity vector.
    struct PlayerView {
        Ogre::Entity* body = nullptr;
        Ogre::Entity* cape = nullptr;
        Ogre::SceneNode* node = nullptr;
        Ogre::SceneNode* cape_node = nullptr;
        float facing = 0.0f;
        uint8_t race_id = 0;
        Vec2 last_pos{0.0f, 0.0f};
        float cape_angle = 0.0f;
        bool pos_valid = false;
    };

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

    /// Builds (or rebuilds) a player's body and cape entities from the cached
    /// race meshes, and returns the view. `initialised` is false until the
    /// first call, so a fresh view is created rather than looked up.
    PlayerView& ensure_player(PlayerView& view, const std::string& name, uint8_t race_id);
    void position_player(PlayerView& view, const Vec2& pos, float angle);
    /// Swings the cape node to trail behind the latest velocity; a no-op when
    /// the view has no cape (the flat fallback).
    void update_cape(PlayerView& view, const Vec2& pos);

    /// The shared cel material for every player's body (the race meshes bake
    /// the palette, so the base colour is white), and the per-race cape
    /// material tinted to the race's accent colour.
    Ogre::MaterialPtr body_material();
    Ogre::MaterialPtr cape_material(uint8_t race_id);

    size_t slot_index(const ColourSlot& slot);
    ColourBatch& batch_for(const ColourSlot& slot);
    void slot_capacity(ColourPool pool, uint32_t& verts, uint32_t& indices);

    /// The Manual Object that holds one draw layer's sections.
    Ogre::ManualObject* pool_object(ColourPool pool);

    /// Adds a section (and its material) for a colour slot on first use. The
    /// EnemyBody, Projectile and Gem pools get the cel-shaded toon material;
    /// the bar, elite-outline and telegraph pools stay flat, since they are
    /// arena-floor and HUD-style elements rather than characters.
    void create_slot(const ColourSlot& slot);

    /// The orthographic viewport is a fixed 80 x 45 units, so the window is
    /// forced to the matching 16:9 aspect ratio to avoid stretching.
    OgreBites::NativeWindowPair createWindow(const Ogre::String& name, uint32_t w, uint32_t h,
                                             Ogre::NameValuePairList miscParams) override;

    bool frameRenderingQueued(const Ogre::FrameEvent& evt) override;

    Ogre::SceneManager* scene_manager_ = nullptr;
    Ogre::Camera* camera_ = nullptr;
    Ogre::SceneNode* camera_node_ = nullptr;
    Ogre::ManualObject* grid_ = nullptr;
    std::unique_ptr<MeshFactory> meshes_;
    std::unique_ptr<CelShaderGenerator> cel_generator_;
    std::unique_ptr<AnthropomorphicMeshBuilder> race_meshes_;
    std::unique_ptr<TelegraphRenderer> telegraphs_;
    InputManager* input_ = nullptr;
    bool cel_ready_ = false;

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

    /// The local player's cel-shaded view.
    PlayerView local_player_;
    Ogre::MaterialPtr body_material_;
    std::unordered_map<uint8_t, Ogre::MaterialPtr> cape_materials_;

    std::function<void(float)> tick_;
    bool quit_ = false;

    std::unordered_map<uint8_t, PlayerView> remote_players_;
};

}  // namespace haven
