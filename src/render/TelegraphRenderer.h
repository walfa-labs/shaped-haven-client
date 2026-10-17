#pragma once

#include "entity_frame.h"

#include <OgreManualObject.h>
#include <OgreMaterial.h>
#include <OgrePrerequisites.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>

#include <vector>

namespace haven {

/// Ground-plane warnings for the attacks the server telegraphs.
///
/// The snapshot carries a windup per enemy (telegraph_progress, telegraph_shape,
/// telegraph_angle, telegraph_radius). Every enemy with progress above zero
/// gets a painted shape on the arena floor: an amber-to-crimson circle for a
/// CircleAoE, a sector for a ConeSlash and a swept rectangle for a LineCharge,
/// sized by telegraph_radius and oriented by telegraph_angle.
///
/// The shapes live in a fixed pool of Manual Objects, one per enemy that can
/// be mid-windup at once. Each slot owns its material, so the pool's colours
/// are per-enemy and per-frame without a rebuild: a slot is geometry-rewritten
/// every frame it is in use and hidden when it is not, which is what keeps an
/// empty windup from leaving last frame's shape on the floor.
///
/// The renderer is driven from HavenRenderer::render_entities, which already
/// holds the frame's interpolated draw items; it allocates nothing per frame.
class TelegraphRenderer {
public:
    explicit TelegraphRenderer(Ogre::SceneManager* scene_manager);
    ~TelegraphRenderer();

    TelegraphRenderer(const TelegraphRenderer&) = delete;
    TelegraphRenderer& operator=(const TelegraphRenderer&) = delete;

    /// Rewrites every in-use slot from the frame's enemies and hides the ones
    /// no enemy needs any more. Enemies without a windup contribute nothing.
    void render(const std::vector<EntityDrawItem>& items);

    /// Destroys the pooled scene objects while Ogre is still alive. Must be
    /// called before the renderer's closeApp(); render() is a no-op afterwards.
    void shutdown();

private:
    struct Slot {
        Ogre::ManualObject* object = nullptr;
        Ogre::MaterialPtr material;
        bool in_use = false;
    };

    /// Points `slot` at one enemy's windup: recolours it by progress and
    /// rewrites its geometry from the telegraph fields.
    void layout_slot(size_t index, const EntityDrawItem& item);
    void hide_slot(size_t index);

    Ogre::SceneManager* scene_ = nullptr;
    std::vector<Slot> slots_;
    bool initialised_ = false;
};

}  // namespace haven
