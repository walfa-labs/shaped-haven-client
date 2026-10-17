#pragma once

#include <OgreColourValue.h>
#include <OgreMaterial.h>
#include <OgreMesh.h>
#include <OgrePrerequisites.h>
#include <OgreSceneManager.h>
#include <OgreVector3.h>

#include <cstdint>
#include <string>

namespace haven {

/// Produces the cell-shaded look: a two-pass material (an inverted-hull ink
/// outline behind a stepped toon diffuse) plus the shader programs it is built
/// from.
///
/// Pass 1 extrudes the mesh's vertices along a baked in-plane "hull"
/// direction, culls the front faces so only the back of the expanded shell
/// shows, and paints it a flat ink colour. Because the meshes this client
/// draws are flat cutouts in the XY plane rather than 3D solids, the hull
/// direction is a per-vertex in-plane offset baked into the mesh's first
/// texture coordinate (see AnthropomorphicMeshBuilder) rather than the
/// geometric normal; the shell is also pushed slightly back in z so the toon
/// pass covers its middle and only the silhouette border reads as ink.
///
/// Pass 2 quantises the lighting into three bands (highlight, midtone,
/// shadow) from a per-vertex "fake" normal — the meshes are flat, so the
/// builder bakes a domed normal per shape instead of a real one — adds a rim
/// light at grazing angles and an optional halftone dot pattern in the shadow
/// band.
///
/// Programs are registered with Ogre's HighLevelGpuProgramManager in both
/// GLSL (OpenGL) and HLSL (Direct3D); only the flavour matching the active
/// render system is loaded. If the shaders fail to compile, initialise()
/// returns false and the caller keeps the flat coloured-primitive rendering
/// the game had before cel-shading.
class CelShaderGenerator {
public:
    explicit CelShaderGenerator(Ogre::SceneManager* scene_manager);
    ~CelShaderGenerator();

    CelShaderGenerator(const CelShaderGenerator&) = delete;
    CelShaderGenerator& operator=(const CelShaderGenerator&) = delete;

    /// Registers the shader programs and compiles the one for the active
    /// render system. Returns false if the programs could not be compiled;
    /// ready() is then false and every factory below returns null, which the
    /// caller reads as "use the flat materials instead".
    bool initialise();
    bool ready() const { return ready_; }

    /// The two-pass cel material for a mesh: inverted-hull outline behind a
    /// toon diffuse. `name` must be unique per (mesh, base colour) pair.
    /// Returns null when not ready. `base_colour` is the toon base colour,
    /// `ink_colour` the outline colour (#0A0A0F for normal foes, #FF0033 for
    /// elites and bosses).
    Ogre::MaterialPtr cel_material(const std::string& name, const Ogre::ColourValue& base_colour,
                                   const Ogre::ColourValue& ink_colour);

    /// The single-pass toon material for the per-frame combat batches, whose
    /// Manual Objects only carry position and normal (no hull direction, no
    /// per-vertex tint). Same banding, rim and halftone as cel_material(), but
    /// no ink outline pass — a thousand enemies stay one draw call each.
    Ogre::MaterialPtr batch_material(const std::string& name, const Ogre::ColourValue& base_colour);

    /// Applies a cel material to a mesh and returns it, ready to assign to the
    /// mesh's submesh. Returns null when not ready.
    Ogre::MaterialPtr apply_to_mesh(Ogre::MeshPtr mesh, const std::string& name,
                                    const Ogre::ColourValue& base_colour,
                                    const Ogre::ColourValue& ink_colour);

    /// The world-space direction the toon bands are lit from, pointing from
    /// the scene toward the light. Defaults to a high front-left key light.
    void set_light_direction(const Ogre::Vector3& dir);
    Ogre::Vector3 light_direction() const { return light_dir_; }

    /// Ink outline width in mesh units.
    void set_outline_width(float width);
    float outline_width() const { return outline_width_; }

    /// The rim light's strength and width (the grazing-angle band's falloff).
    void set_rim(float strength, float width);
    /// Whether the shadow band carries the halftone dot pattern.
    void set_halftone(bool enabled);

    Ogre::SceneManager* scene_manager() const { return scene_; }

private:
    /// The shader sources, as string constants. Kept next to the loader so the
    /// GLSL and HLSL flavours of one program are easy to compare.
    static const char* outline_vertex_glsl();
    static const char* outline_fragment_glsl();
    static const char* toon_vertex_glsl();
    static const char* toon_fragment_glsl();
    static const char* batch_vertex_glsl();

    static const char* outline_vertex_hlsl();
    static const char* outline_fragment_hlsl();
    static const char* toon_vertex_hlsl();
    static const char* toon_fragment_hlsl();
    static const char* batch_vertex_hlsl();

    /// Creates and loads the programs for the active render system's shading
    /// language. Returns false if any program fails to compile.
    bool create_programs();
    /// True when the active render system speaks GLSL rather than HLSL.
    bool render_system_is_gl() const;

    Ogre::GpuProgramPtr make_program(const std::string& name, const char* source, bool vertex);

    /// Fills a pass with the material-wide named constants (light, rim,
    /// halftone). `base_colour` is the toon base, `ink` the outline colour.
    void configure_pass(Ogre::Pass* pass, const Ogre::ColourValue& base, const Ogre::ColourValue& ink,
                         bool outline);

    Ogre::SceneManager* scene_ = nullptr;
    bool ready_ = false;
    bool is_gl_ = true;

    std::string outline_vs_;
    std::string outline_fs_;
    std::string toon_vs_;
    std::string toon_fs_;
    std::string batch_vs_;

    Ogre::Vector3 light_dir_{-0.45f, 0.62f, 0.78f};
    float outline_width_ = 0.035f;
    float rim_strength_ = 0.55f;
    float rim_width_ = 0.55f;
    bool halftone_ = true;
};

}  // namespace haven
