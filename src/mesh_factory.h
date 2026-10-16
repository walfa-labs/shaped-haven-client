#pragma once

#include <OgreColourValue.h>
#include <OgreMaterial.h>
#include <OgreMesh.h>
#include <OgrePrerequisites.h>
#include <OgreSceneManager.h>

#include <cstdint>
#include <string>

namespace haven {

enum class Shape : uint8_t {
    Square = 0,
    Circle = 1,
    Triangle = 2,
};

/// Builds every mesh the client needs from code. There are no art assets, so
/// the player square, the enemy circle/triangle primitives and the arena grid
/// are all generated here at startup.
class MeshFactory {
public:
    explicit MeshFactory(Ogre::SceneManager* scene_manager);

    /// `radius` is the half-extent for Square and the circumradius for Circle
    /// and Triangle. Meshes are centred on the origin in the XY plane.
    Ogre::MeshPtr build(Shape shape, float radius, const std::string& name);

    /// Unlit flat-colour material, the only kind phase 1 needs.
    Ogre::MaterialPtr flat_material(const std::string& name, const Ogre::ColourValue& colour);

    Ogre::SceneManager* scene_manager() const { return scene_manager_; }

private:
    void build_square(float radius, Ogre::MeshPtr& mesh, Ogre::SubMesh* sub);
    void build_circle(float radius, Ogre::MeshPtr& mesh, Ogre::SubMesh* sub, uint32_t segments);
    void build_triangle(float radius, Ogre::MeshPtr& mesh, Ogre::SubMesh* sub);

    Ogre::SceneManager* scene_manager_ = nullptr;
};

}  // namespace haven
