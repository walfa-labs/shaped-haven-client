#pragma once

#include "game_math.h"
#include "input_state.h"
#include "mesh_factory.h"

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

    /// Local player square, driven by the prediction state.
    void set_local_player(const Vec2& pos);

    /// Remote players, driven by the interpolation buffer.
    void set_remote_player(uint8_t player_id, const Vec2& pos);
    void remove_player(uint8_t player_id);
    void clear_remote_players();

private:
    void createRoot();
    bool oneTimeConfig();
    void locateResources();
    void loadResources();
    void setup();

    void build_scene();
    void build_grid();
    void build_camera();

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

    std::function<void(float)> tick_;
    bool quit_ = false;

    std::unordered_map<uint8_t, Ogre::SceneNode*> remote_players_;
};

}  // namespace haven
