#include "render/CelShaderGenerator.h"

#include <OgreGpuProgram.h>
#include <OgreGpuProgramManager.h>
#include <OgreHighLevelGpuProgramManager.h>
#include <OgreLogManager.h>
#include <OgreMaterialManager.h>
#include <OgrePass.h>
#include <OgreRoot.h>
#include <OgreSubMesh.h>
#include <OgreTechnique.h>

#include <cstdio>

namespace haven {

namespace {

constexpr const char* kResourceGroup = "ShapedHaven";

/// The outline shell is pushed this far back in z so the toon pass covers its
/// middle and only the silhouette border reads as ink. Orthographic depth is
/// linear, so a fixed offset is enough at the 80 x 45 viewport.
constexpr float kOutlineZOffset = 0.004f;

Ogre::String to_string(const Ogre::ColourValue& c)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "(%.3f, %.3f, %.3f, %.3f)", static_cast<double>(c.r),
                  static_cast<double>(c.g), static_cast<double>(c.b), static_cast<double>(c.a));
    return Ogre::String(buf);
}

}  // namespace

// ---------------------------------------------------------------------------
// Shader sources.
//
// The meshes this client draws are flat cutouts in the XY plane under a
// top-down orthographic camera, so the "normals" are fake: the mesh builder
// bakes a domed normal per shape (radial from the shape's centroid, tilted
// toward the camera) into the normal attribute, and the toon bands quantise
// the dot of that normal with a fixed key light. The outline pass extrudes
// along a per-vertex in-plane hull direction baked into the first texture
// coordinate, because a flat mesh's geometric normal (0,0,1) would extrude
// toward the camera instead of expanding the silhouette.
//
// Automatic (Ogre-bound) uniforms use the snake_case names GpuProgramParameters
// derives from the AutoConstantType enum: world_matrix, view_matrix,
// projection_matrix, camera_position. The rest are set as named constants.
// ---------------------------------------------------------------------------

const char* CelShaderGenerator::outline_vertex_glsl()
{
    return
        "OGRE_NATIVE_GLSL_VERSION_DIRECTIVE\n"
        "\n"
        "in vec4 vertex;\n"
        "in vec3 normal;\n"
        "in vec2 uv0;\n"
        "in vec4 colour;\n"
        "\n"
        "uniform mat4 world_matrix;\n"
        "uniform mat4 view_matrix;\n"
        "uniform mat4 projection_matrix;\n"
        "uniform float outline_width;\n"
        "\n"
        "out vec4 vColour;\n"
        "\n"
        "void main()\n"
        "{\n"
        "    vec3 pos = vertex.xyz;\n"
        "    pos.xy += uv0 * outline_width;\n"
        "    pos.z -= 0.004;\n"
        "    vec4 worldPos = world_matrix * vec4(pos, 1.0);\n"
        "    vColour = colour;\n"
        "    gl_Position = projection_matrix * view_matrix * worldPos;\n"
        "}\n";
}

const char* CelShaderGenerator::outline_fragment_glsl()
{
    return
        "OGRE_NATIVE_GLSL_VERSION_DIRECTIVE\n"
        "\n"
        "in vec4 vColour;\n"
        "\n"
        "uniform vec3 ink_colour;\n"
        "\n"
        "out vec4 fragColour;\n"
        "\n"
        "void main()\n"
        "{\n"
        "    fragColour = vec4(ink_colour, vColour.a);\n"
        "}\n";
}

const char* CelShaderGenerator::toon_vertex_glsl()
{
    return
        "OGRE_NATIVE_GLSL_VERSION_DIRECTIVE\n"
        "\n"
        "in vec4 vertex;\n"
        "in vec3 normal;\n"
        "in vec2 uv0;\n"
        "in vec2 uv1;\n"
        "in vec4 colour;\n"
        "\n"
        "uniform mat4 world_matrix;\n"
        "uniform mat4 view_matrix;\n"
        "uniform mat4 projection_matrix;\n"
        "\n"
        "out vec3 vNormalW;\n"
        "out vec3 vPosW;\n"
        "out vec4 vColour;\n"
        "out float vGlow;\n"
        "\n"
        "void main()\n"
        "{\n"
        "    vec4 worldPos = world_matrix * vertex;\n"
        "    vNormalW = mat3(world_matrix) * normal;\n"
        "    vPosW = worldPos.xyz;\n"
        "    vColour = colour;\n"
        "    vGlow = uv1.x;\n"
        "    gl_Position = projection_matrix * view_matrix * worldPos;\n"
        "}\n";
}

const char* CelShaderGenerator::toon_fragment_glsl()
{
    return
        "OGRE_NATIVE_GLSL_VERSION_DIRECTIVE\n"
        "\n"
        "in vec3 vNormalW;\n"
        "in vec3 vPosW;\n"
        "in vec4 vColour;\n"
        "in float vGlow;\n"
        "\n"
        "uniform vec3 light_dir;\n"
        "uniform vec3 camera_position;\n"
        "uniform vec3 base_colour;\n"
        "uniform float rim_strength;\n"
        "uniform float rim_width;\n"
        "uniform float halftone;\n"
        "\n"
        "out vec4 fragColour;\n"
        "\n"
        "void main()\n"
        "{\n"
        "    vec3 n = normalize(vNormalW);\n"
        "    vec3 l = normalize(light_dir);\n"
        "    float ndl = dot(n, l) * 0.5 + 0.5;\n"
        "\n"
        "    float band = 0.0;\n"
        "    if (ndl > 0.62) band = 1.0;\n"
        "    else if (ndl > 0.34) band = 0.5;\n"
        "\n"
        "    vec3 shadowTint = base_colour * 0.30;\n"
        "    vec3 midTint = base_colour * 0.74;\n"
        "    vec3 highTint = base_colour * 1.10 + vec3(0.05);\n"
        "    vec3 col = band > 0.9 ? highTint : (band > 0.1 ? midTint : shadowTint);\n"
        "    col *= vColour.rgb;\n"
        "\n"
        "    vec3 v = normalize(camera_position - vPosW);\n"
        "    float rim = 1.0 - max(dot(n, v), 0.0);\n"
        "    rim = smoothstep(1.0 - rim_width, 1.0, rim);\n"
        "    col += rim * rim_strength * vec3(1.0, 0.97, 0.90);\n"
        "\n"
        "    col += vGlow * (vColour.rgb * 0.8 + vec3(0.25));\n"
        "\n"
        "    if (halftone > 0.5 && band < 0.1)\n"
        "    {\n"
        "        vec2 cell = fract(gl_FragCoord.xy / 7.0) - 0.5;\n"
        "        float mask = step(length(cell), 0.30);\n"
        "        col *= 1.0 - mask * 0.40;\n"
        "    }\n"
        "\n"
        "    fragColour = vec4(col, vColour.a);\n"
        "}\n";
}

const char* CelShaderGenerator::batch_vertex_glsl()
{
    return
        "OGRE_NATIVE_GLSL_VERSION_DIRECTIVE\n"
        "\n"
        "in vec4 vertex;\n"
        "in vec3 normal;\n"
        "\n"
        "uniform mat4 world_matrix;\n"
        "uniform mat4 view_matrix;\n"
        "uniform mat4 projection_matrix;\n"
        "\n"
        "out vec3 vNormalW;\n"
        "out vec3 vPosW;\n"
        "out vec4 vColour;\n"
        "out float vGlow;\n"
        "\n"
        "void main()\n"
        "{\n"
        "    vec4 worldPos = world_matrix * vertex;\n"
        "    vNormalW = mat3(world_matrix) * normal;\n"
        "    vPosW = worldPos.xyz;\n"
        "    vColour = vec4(1.0, 1.0, 1.0, 1.0);\n"
        "    vGlow = 0.0;\n"
        "    gl_Position = projection_matrix * view_matrix * worldPos;\n"
        "}\n";
}

// ---------------------------------------------------------------------------
// HLSL flavours, for Direct3D. Ogre binds the same snake_case automatic
// constant names on every render system, so the uniform names match the GLSL.
// ---------------------------------------------------------------------------

const char* CelShaderGenerator::outline_vertex_hlsl()
{
    return
        "float4x4 world_matrix;\n"
        "float4x4 view_matrix;\n"
        "float4x4 projection_matrix;\n"
        "float outline_width;\n"
        "\n"
        "struct VS_INPUT\n"
        "{\n"
        "    float4 vertex : POSITION;\n"
        "    float3 normal : NORMAL;\n"
        "    float2 uv0 : TEXCOORD0;\n"
        "    float4 colour : COLOR0;\n"
        "};\n"
        "struct VS_OUTPUT\n"
        "{\n"
        "    float4 position : POSITION;\n"
        "    float4 colour : COLOR0;\n"
        "};\n"
        "\n"
        "VS_OUTPUT main(VS_INPUT input)\n"
        "{\n"
        "    VS_OUTPUT output;\n"
        "    float3 pos = input.vertex.xyz;\n"
        "    pos.xy += input.uv0 * outline_width;\n"
        "    pos.z -= 0.004;\n"
        "    float4 worldPos = mul(world_matrix, float4(pos, 1.0));\n"
        "    output.position = mul(projection_matrix, mul(view_matrix, worldPos));\n"
        "    output.colour = input.colour;\n"
        "    return output;\n"
        "}\n";
}

const char* CelShaderGenerator::outline_fragment_hlsl()
{
    return
        "float3 ink_colour;\n"
        "\n"
        "struct FS_INPUT\n"
        "{\n"
        "    float4 colour : COLOR0;\n"
        "};\n"
        "\n"
        "float4 main(FS_INPUT input) : SV_Target0\n"
        "{\n"
        "    return float4(ink_colour, input.colour.a);\n"
        "}\n";
}

const char* CelShaderGenerator::toon_vertex_hlsl()
{
    return
        "float4x4 world_matrix;\n"
        "float4x4 view_matrix;\n"
        "float4x4 projection_matrix;\n"
        "\n"
        "struct VS_INPUT\n"
        "{\n"
        "    float4 vertex : POSITION;\n"
        "    float3 normal : NORMAL;\n"
        "    float2 uv0 : TEXCOORD0;\n"
        "    float2 uv1 : TEXCOORD1;\n"
        "    float4 colour : COLOR0;\n"
        "};\n"
        "struct VS_OUTPUT\n"
        "{\n"
        "    float4 position : POSITION;\n"
        "    float3 normal : TEXCOORD0;\n"
        "    float3 posW : TEXCOORD1;\n"
        "    float4 colour : TEXCOORD2;\n"
        "    float glow : TEXCOORD3;\n"
        "};\n"
        "\n"
        "VS_OUTPUT main(VS_INPUT input)\n"
        "{\n"
        "    VS_OUTPUT output;\n"
        "    float4 worldPos = mul(world_matrix, input.vertex);\n"
        "    output.normal = mul((float3x3)world_matrix, input.normal);\n"
        "    output.posW = worldPos.xyz;\n"
        "    output.colour = input.colour;\n"
        "    output.glow = input.uv1.x;\n"
        "    output.position = mul(projection_matrix, mul(view_matrix, worldPos));\n"
        "    return output;\n"
        "}\n";
}

const char* CelShaderGenerator::toon_fragment_hlsl()
{
    return
        "float3 light_dir;\n"
        "float3 camera_position;\n"
        "float3 base_colour;\n"
        "float rim_strength;\n"
        "float rim_width;\n"
        "float halftone;\n"
        "\n"
        "struct FS_INPUT\n"
        "{\n"
        "    float3 normal : TEXCOORD0;\n"
        "    float3 posW : TEXCOORD1;\n"
        "    float4 colour : TEXCOORD2;\n"
        "    float glow : TEXCOORD3;\n"
        "    float2 pixel : VPOS;\n"
        "};\n"
        "\n"
        "float4 main(FS_INPUT input) : SV_Target0\n"
        "{\n"
        "    float3 n = normalize(input.normal);\n"
        "    float3 l = normalize(light_dir);\n"
        "    float ndl = dot(n, l) * 0.5 + 0.5;\n"
        "\n"
        "    float band = 0.0;\n"
        "    if (ndl > 0.62) band = 1.0;\n"
        "    else if (ndl > 0.34) band = 0.5;\n"
        "\n"
        "    float3 shadowTint = base_colour * 0.30;\n"
        "    float3 midTint = base_colour * 0.74;\n"
        "    float3 highTint = base_colour * 1.10 + float3(0.05, 0.05, 0.05);\n"
        "    float3 col = band > 0.9 ? highTint : (band > 0.1 ? midTint : shadowTint);\n"
        "    col *= input.colour.rgb;\n"
        "\n"
        "    float3 v = normalize(camera_position - input.posW);\n"
        "    float rim = 1.0 - max(dot(n, v), 0.0);\n"
        "    rim = smoothstep(1.0 - rim_width, 1.0, rim);\n"
        "    col += rim * rim_strength * float3(1.0, 0.97, 0.90);\n"
        "\n"
        "    col += input.glow * (input.colour.rgb * 0.8 + float3(0.25, 0.25, 0.25));\n"
        "\n"
        "    if (halftone > 0.5 && band < 0.1)\n"
        "    {\n"
        "        float2 cell = frac(input.pixel.xy / 7.0) - 0.5;\n"
        "        float mask = step(length(cell), 0.30);\n"
        "        col *= 1.0 - mask * 0.40;\n"
        "    }\n"
        "\n"
        "    return float4(col, input.colour.a);\n"
        "}\n";
}

const char* CelShaderGenerator::batch_vertex_hlsl()
{
    return
        "float4x4 world_matrix;\n"
        "float4x4 view_matrix;\n"
        "float4x4 projection_matrix;\n"
        "\n"
        "struct VS_INPUT\n"
        "{\n"
        "    float4 vertex : POSITION;\n"
        "    float3 normal : NORMAL;\n"
        "};\n"
        "struct VS_OUTPUT\n"
        "{\n"
        "    float4 position : POSITION;\n"
        "    float3 normal : TEXCOORD0;\n"
        "    float3 posW : TEXCOORD1;\n"
        "    float4 colour : TEXCOORD2;\n"
        "    float glow : TEXCOORD3;\n"
        "};\n"
        "\n"
        "VS_OUTPUT main(VS_INPUT input)\n"
        "{\n"
        "    VS_OUTPUT output;\n"
        "    float4 worldPos = mul(world_matrix, input.vertex);\n"
        "    output.normal = mul((float3x3)world_matrix, input.normal);\n"
        "    output.posW = worldPos.xyz;\n"
        "    output.colour = float4(1.0, 1.0, 1.0, 1.0);\n"
        "    output.glow = 0.0;\n"
        "    output.position = mul(projection_matrix, mul(view_matrix, worldPos));\n"
        "    return output;\n"
        "}\n";
}

// ---------------------------------------------------------------------------
// Construction and program creation.
// ---------------------------------------------------------------------------

CelShaderGenerator::CelShaderGenerator(Ogre::SceneManager* scene_manager) : scene_(scene_manager) {}

CelShaderGenerator::~CelShaderGenerator() = default;

bool CelShaderGenerator::render_system_is_gl() const
{
    Ogre::Root& root = Ogre::Root::getSingleton();
    if (root.getRenderSystem() == nullptr) return true;
    const Ogre::String& name = root.getRenderSystem()->getName();
    return name.find("GL") != Ogre::String::npos || name.find("OpenGL") != Ogre::String::npos ||
           name.find("Vulkan") != Ogre::String::npos;
}

Ogre::GpuProgramPtr CelShaderGenerator::make_program(const std::string& name, const char* source, bool vertex)
{
    const Ogre::String language = is_gl_ ? "glsl" : "hlsl";
    Ogre::GpuProgramPtr program = Ogre::HighLevelGpuProgramManager::getSingleton().createProgram(
        name, kResourceGroup, language, vertex ? Ogre::GPT_VERTEX_PROGRAM : Ogre::GPT_FRAGMENT_PROGRAM);
    if (program == nullptr)
    {
        std::fprintf(stderr, "haven: could not create %s program %s\n", language.c_str(), name.c_str());
        return nullptr;
    }
    program->setSource(source);
    if (!is_gl_)
    {
        program->setParameter("target", vertex ? "vs_4_0" : "ps_4_0");
    }
    program->load();
    if (!program->isLoaded() || program->hasCompileError())
    {
        std::fprintf(stderr, "haven: %s program %s failed to compile (see shaped-haven.log)\n",
                     language.c_str(), name.c_str());
        return nullptr;
    }
    return program;
}

bool CelShaderGenerator::create_programs()
{
    const bool gl = render_system_is_gl();
    is_gl_ = gl;

    Ogre::GpuProgramPtr outline_vs = make_program("haven/cel_outline_vs", gl ? outline_vertex_glsl() : outline_vertex_hlsl(), true);
    Ogre::GpuProgramPtr outline_fs = make_program("haven/cel_outline_fs", gl ? outline_fragment_glsl() : outline_fragment_hlsl(), false);
    Ogre::GpuProgramPtr toon_vs = make_program("haven/cel_toon_vs", gl ? toon_vertex_glsl() : toon_vertex_hlsl(), true);
    Ogre::GpuProgramPtr toon_fs = make_program("haven/cel_toon_fs", gl ? toon_fragment_glsl() : toon_fragment_hlsl(), false);
    Ogre::GpuProgramPtr batch_vs = make_program("haven/cel_batch_vs", gl ? batch_vertex_glsl() : batch_vertex_hlsl(), true);

    if (outline_vs.isNull() || outline_fs.isNull() || toon_vs.isNull() || toon_fs.isNull() || batch_vs.isNull())
        return false;

    outline_vs_ = outline_vs->getName();
    outline_fs_ = outline_fs->getName();
    toon_vs_ = toon_vs->getName();
    toon_fs_ = toon_fs->getName();
    batch_vs_ = batch_vs->getName();
    return true;
}

bool CelShaderGenerator::initialise()
{
    if (scene_ == nullptr) return false;
    ready_ = create_programs();
    if (!ready_)
    {
        std::fprintf(stderr, "haven: cel-shading shaders unavailable, using flat materials\n");
    }
    return ready_;
}

// ---------------------------------------------------------------------------
// Materials.
// ---------------------------------------------------------------------------

void CelShaderGenerator::configure_pass(Ogre::Pass* pass, const Ogre::ColourValue& base,
                                        const Ogre::ColourValue& ink, bool outline)
{
    if (pass == nullptr) return;
    Ogre::GpuProgramParametersSharedPtr params = outline ? pass->getVertexProgramParameters()
                                                          : pass->getFragmentProgramParameters();
    if (params == nullptr) return;

    if (outline)
    {
        params->setNamedConstant("outline_width", outline_width_);
        pass->getFragmentProgramParameters()->setNamedConstant("ink_colour", ink);
    }
    else
    {
        params->setNamedConstant("light_dir", light_dir_);
        params->setNamedConstant("base_colour", base);
        params->setNamedConstant("rim_strength", rim_strength_);
        params->setNamedConstant("rim_width", rim_width_);
        params->setNamedConstant("halftone", halftone_ ? 1.0f : 0.0f);
    }
}

Ogre::MaterialPtr CelShaderGenerator::cel_material(const std::string& name, const Ogre::ColourValue& base_colour,
                                                   const Ogre::ColourValue& ink_colour)
{
    if (!ready_) return nullptr;

    Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().create(name, kResourceGroup);
    Ogre::Technique* technique = material->getTechnique(0);

    // Pass 1: the inverted-hull ink outline. Front-face culling leaves only the
    // back of the expanded shell showing; the toon pass covers its middle.
    Ogre::Pass* outline = technique->createPass();
    outline->setVertexProgram(outline_vs_);
    outline->setFragmentProgram(outline_fs_);
    outline->setCullingMode(Ogre::CULL_ANTICLOCKWISE);
    outline->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
    configure_pass(outline, base_colour, ink_colour, true);

    // Pass 2: the stepped toon diffuse with rim light and halftone.
    Ogre::Pass* toon = technique->createPass();
    toon->setVertexProgram(toon_vs_);
    toon->setFragmentProgram(toon_fs_);
    toon->setCullingMode(Ogre::CULL_CLOCKWISE);
    toon->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
    configure_pass(toon, base_colour, ink_colour, false);

    material->load();
    return material;
}

Ogre::MaterialPtr CelShaderGenerator::batch_material(const std::string& name,
                                                     const Ogre::ColourValue& base_colour)
{
    if (!ready_) return nullptr;

    Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().create(name, kResourceGroup);
    Ogre::Technique* technique = material->getTechnique(0);

    // One pass: the toon diffuse. No outline — the combat batches hold up to a
    // thousand enemies in one draw call each, and a second pass would double
    // that. The banding, rim and halftone still read as cel-shading.
    Ogre::Pass* toon = technique->getPass(0);
    toon->setVertexProgram(batch_vs_);
    toon->setFragmentProgram(toon_fs_);
    toon->setCullingMode(Ogre::CULL_CLOCKWISE);
    configure_pass(toon, base_colour, Ogre::ColourValue::Black, false);

    material->load();
    return material;
}

Ogre::MaterialPtr CelShaderGenerator::apply_to_mesh(Ogre::MeshPtr mesh, const std::string& name,
                                                    const Ogre::ColourValue& base_colour,
                                                    const Ogre::ColourValue& ink_colour)
{
    Ogre::MaterialPtr material = cel_material(name, base_colour, ink_colour);
    if (material.isNull() || mesh.isNull()) return nullptr;
    if (mesh->getNumSubMeshes() > 0)
    {
        mesh->getSubMesh(0)->setMaterialName(material->getName());
    }
    return material;
}

void CelShaderGenerator::set_light_direction(const Ogre::Vector3& dir)
{
    light_dir_ = dir.normalisedCopy();
}

void CelShaderGenerator::set_outline_width(float width)
{
    outline_width_ = width;
}

void CelShaderGenerator::set_rim(float strength, float width)
{
    rim_strength_ = strength;
    rim_width_ = width;
}

void CelShaderGenerator::set_halftone(bool enabled)
{
    halftone_ = enabled;
}

}  // namespace haven
