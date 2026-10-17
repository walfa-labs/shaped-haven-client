#pragma once

#include <OgreColourValue.h>
#include <OgreMesh.h>
#include <OgrePrerequisites.h>
#include <OgreSceneManager.h>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace haven {

/// Builds the procedural anthropomorphic meshes for the ten playable races.
///
/// Every mesh is a flat cutout in the XY plane (the game is top-down, so the
/// characters read as stylised paper-doll silhouettes), facing +X with the
/// head toward +Y. The builder bakes the per-vertex data the cel-shading
/// shaders need, because the meshes are flat and have no real normals or
/// outlines to derive at render time:
///
///  - a fake "domed" normal per vertex, radial from the shape's centroid and
///    tilted toward the camera, which the toon shader quantises into bands;
///  - an in-plane hull direction per vertex (the first texture coordinate),
///    which the outline pass extrudes along to expand the silhouette;
///  - a per-vertex colour (the race's palette) and glow flag for visors and
///    ocular gems, and an alpha for translucent parts.
///
/// Meshes are built once per race and cached, so the render loop never
/// rebuilds them. The cape/scarf is a separate mesh from the body so it can
/// sit on its own scene node and trail behind the velocity vector.
class AnthropomorphicMeshBuilder {
public:
    explicit AnthropomorphicMeshBuilder(Ogre::SceneManager* scene_manager);

    /// The race's body mesh, centred on the origin in the XY plane and facing
    /// +X. Built on first use and cached; the pointer is stable.
    Ogre::MeshPtr body_mesh_for_race(uint8_t race_id);

    /// The race's cape/scarf mesh, a small trailing shape behind the body
    /// (-X). Also cached. Some races have no cape and share an empty mesh.
    Ogre::MeshPtr cape_mesh_for_race(uint8_t race_id);

    /// The race's palette, for callers that want to tint or preview it.
    static Ogre::ColourValue race_body_colour(uint8_t race_id);
    static Ogre::ColourValue race_accent_colour(uint8_t race_id);

    Ogre::SceneManager* scene_manager() const { return scene_; }

private:
    /// One part's baked colour, glow and alpha.
    struct PartStyle {
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 1.0f;
        float glow = 0.0f;
    };

    Ogre::MeshPtr build_body_mesh(uint8_t race_id);
    Ogre::MeshPtr build_cape_mesh(uint8_t race_id);

    Ogre::SceneManager* scene_ = nullptr;
    std::unordered_map<uint8_t, Ogre::MeshPtr> bodies_;
    std::unordered_map<uint8_t, Ogre::MeshPtr> capes_;
};

}  // namespace haven
