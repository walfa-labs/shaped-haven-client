#include "render/AnthropomorphicMeshBuilder.h"

#include <OgreHardwareBufferManager.h>
#include <OgreHardwareIndexBuffer.h>
#include <OgreMaterialManager.h>
#include <OgreMeshManager.h>
#include <OgreSubMesh.h>
#include <OgreVertexIndexData.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace haven {

namespace {

constexpr const char* kResourceGroup = "ShapedHaven";

constexpr float kPi = 3.14159265358979323846f;

/// Interleaved vertex layout shared by every race mesh, matching the cel
/// shader's attribute bindings: position, the fake domed normal, the in-plane
/// hull direction, the glow flag, and the per-part colour.
constexpr size_t kFloatsPerVertex = 14;
constexpr size_t kVertexSize = kFloatsPerVertex * sizeof(float);

// Offsets into one vertex, in bytes.
constexpr size_t kOffPosition = 0;
constexpr size_t kOffNormal = 12;
constexpr size_t kOffHull = 24;
constexpr size_t kOffGlow = 32;
constexpr size_t kOffColour = 40;

Ogre::ColourValue colour(float r, float g, float b, float a = 1.0f)
{
    return Ogre::ColourValue(r, g, b, a);
}

/// The ten races' palettes: a body colour, an accent colour for details and
/// capes, and a glow colour for visors and ocular gems. High-contrast anime
/// colours with a crimson or cool accent, matching the HUD's Persona palette.
struct Palette {
    Ogre::ColourValue body;
    Ogre::ColourValue accent;
    Ogre::ColourValue glow;
};

const Palette& race_palette(uint8_t race_id)
{
    // Indexed by race id; an unknown id reads as the first race.
    static const std::array<Palette, 10> kPalettes{{
        {colour(0.36f, 0.47f, 0.68f), colour(0.20f, 0.85f, 1.00f), colour(0.30f, 0.95f, 1.00f)},  // Terran Kin
        {colour(0.27f, 0.66f, 0.38f), colour(0.55f, 0.85f, 0.30f), colour(0.85f, 1.00f, 0.30f)},  // Mantid Vex
        {colour(0.22f, 0.56f, 0.62f), colour(0.62f, 0.42f, 0.95f), colour(0.70f, 0.50f, 1.00f)},  // Cephalon Tide
        {colour(0.93f, 0.88f, 0.66f), colour(0.98f, 0.75f, 0.20f), colour(0.30f, 0.90f, 1.00f)},  // Aerie Seraph
        {colour(0.47f, 0.52f, 0.27f), colour(0.95f, 0.55f, 0.20f), colour(1.00f, 0.65f, 0.25f)},  // Saurian Scute
        {colour(0.16f, 0.31f, 0.62f), colour(0.25f, 0.80f, 0.95f), colour(0.35f, 0.90f, 1.00f)},  // Abyssal Pelag
        {colour(0.32f, 0.16f, 0.42f), colour(0.90f, 0.10f, 0.25f), colour(1.00f, 0.20f, 0.40f)},  // Weave Widow
        {colour(0.26f, 0.23f, 0.29f), colour(0.98f, 0.70f, 0.20f), colour(1.00f, 0.80f, 0.30f)},  // Felis Nyx
        {colour(0.46f, 0.41f, 0.36f), colour(0.25f, 0.80f, 0.95f), colour(0.40f, 0.90f, 1.00f)},  // Canid Vanguard
        {colour(0.31f, 0.51f, 0.31f), colour(0.60f, 0.90f, 0.95f), colour(0.75f, 0.98f, 1.00f)},  // Cervid Sylvan
    }};
    return kPalettes[static_cast<size_t>(race_id) % kPalettes.size()];
}

/// Accumulates the interleaved vertices and indices of one flat mesh and
/// bakes them into an Ogre mesh. The primitives below emit counter-clockwise
/// triangles (Ogre's front face) and bake the fake normal and hull direction
/// per vertex from the shape's own centroid, so every part domes and inks the
/// way the cel shaders expect.
class MeshBuilder {
public:
    struct Part {
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 1.0f;
        float glow = 0.0f;
    };

    /// Adds one vertex, baking the fake domed normal (radial from the shape
    /// centroid, tilted toward the camera) and the in-plane hull direction.
    uint32_t vertex(float x, float y, float z, float cx, float cy, float radius, const Part& part)
    {
        const float dx = x - cx;
        const float dy = y - cy;
        const float nz = radius * 0.9f;
        const float inv = 1.0f / std::sqrt(dx * dx + dy * dy + nz * nz);
        const float len = std::sqrt(dx * dx + dy * dy);
        const float hx = len > 1e-6f ? dx / len : 0.0f;
        const float hy = len > 1e-6f ? dy / len : 0.0f;

        verts_.push_back(x);
        verts_.push_back(y);
        verts_.push_back(z);
        verts_.push_back(dx * inv);
        verts_.push_back(dy * inv);
        verts_.push_back(nz * inv);
        verts_.push_back(hx);
        verts_.push_back(hy);
        verts_.push_back(part.glow);
        verts_.push_back(0.0f);
        verts_.push_back(part.r);
        verts_.push_back(part.g);
        verts_.push_back(part.b);
        verts_.push_back(part.a);
        return static_cast<uint32_t>(verts_.size() / kFloatsPerVertex) - 1;
    }

    void triangle(uint32_t a, uint32_t b, uint32_t c)
    {
        indices_.push_back(a);
        indices_.push_back(b);
        indices_.push_back(c);
    }

    /// A triangle with its winding corrected to counter-clockwise, so it is
    /// never culled by the toon pass's front-face culling.
    void tri(float x0, float y0, float x1, float y1, float x2, float y2, float z, float cx, float cy,
             float radius, const Part& part)
    {
        if ((x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0) < 0.0f)
        {
            std::swap(x1, x2);
            std::swap(y1, y2);
        }
        const uint32_t a = vertex(x0, y0, z, cx, cy, radius, part);
        const uint32_t b = vertex(x1, y1, z, cx, cy, radius, part);
        const uint32_t c = vertex(x2, y2, z, cx, cy, radius, part);
        triangle(a, b, c);
    }

    /// A regular polygon fan: a centre vertex plus a `sides` vertex ring.
    void fan(float cx, float cy, float radius, uint32_t sides, float rot, float z, const Part& part)
    {
        const uint32_t centre = vertex(cx, cy, z, cx, cy, radius, part);
        std::vector<uint32_t> ring;
        ring.reserve(sides + 1);
        for (uint32_t i = 0; i <= sides; ++i)
        {
            const float a = rot + static_cast<float>(i) * (2.0f * kPi / static_cast<float>(sides));
            ring.push_back(vertex(cx + radius * std::cos(a), cy + radius * std::sin(a), z, cx, cy, radius, part));
        }
        for (uint32_t i = 0; i < sides; ++i)
            triangle(centre, ring[i], ring[i + 1]);
    }

    /// A rectangle rotated by `angle`, for limbs and pauldrons.
    void quad(float cx, float cy, float hx, float hy, float angle, float z, const Part& part)
    {
        const float ca = std::cos(angle);
        const float sa = std::sin(angle);
        const float corners[4][2] = {{-hx, -hy}, {hx, -hy}, {hx, hy}, {-hx, hy}};
        const float radius = std::sqrt(hx * hx + hy * hy);
        std::array<uint32_t, 4> idx;
        for (uint32_t i = 0; i < 4; ++i)
        {
            const float lx = corners[i][0];
            const float ly = corners[i][1];
            idx[i] = vertex(cx + lx * ca - ly * sa, cy + lx * sa + ly * ca, z, cx, cy, radius, part);
        }
        triangle(idx[0], idx[1], idx[2]);
        triangle(idx[0], idx[2], idx[3]);
    }

    /// A quad from four explicit corners, for tapered torsos and blades.
    void quad4(const std::array<float, 4>& xs, const std::array<float, 4>& ys, float z, const Part& part)
    {
        const float cx = (xs[0] + xs[1] + xs[2] + xs[3]) * 0.25f;
        const float cy = (ys[0] + ys[1] + ys[2] + ys[3]) * 0.25f;
        float radius = 0.0f;
        for (uint32_t i = 0; i < 4; ++i)
            radius = std::max(radius, std::sqrt((xs[i] - cx) * (xs[i] - cx) + (ys[i] - cy) * (ys[i] - cy)));
        std::array<uint32_t, 4> idx;
        for (uint32_t i = 0; i < 4; ++i)
            idx[i] = vertex(xs[i], ys[i], z, cx, cy, radius, part);
        // Two triangles, each wound counter-clockwise.
        tri(xs[0], ys[0], xs[1], ys[1], xs[2], ys[2], z, cx, cy, radius, part);
        tri(xs[0], ys[0], xs[2], ys[2], xs[3], ys[3], z, cx, cy, radius, part);
    }

    Ogre::MeshPtr bake(const std::string& name)
    {
        Ogre::MeshPtr mesh = Ogre::MeshManager::getSingleton().createManual(name, kResourceGroup);
        Ogre::SubMesh* sub = mesh->createSubMesh();

        Ogre::VertexData* vertex_data = new Ogre::VertexData();
        vertex_data->vertexStart = 0;
        vertex_data->vertexCount = static_cast<unsigned>(verts_.size() / kFloatsPerVertex);

        Ogre::VertexDeclaration* decl = vertex_data->vertexDeclaration;
        decl->addElement(0, kOffPosition, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
        decl->addElement(0, kOffNormal, Ogre::VET_FLOAT3, Ogre::VES_NORMAL);
        decl->addElement(0, kOffHull, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES, 0);
        decl->addElement(0, kOffGlow, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES, 1);
        decl->addElement(0, kOffColour, Ogre::VET_FLOAT4, Ogre::VES_COLOUR);

        Ogre::HardwareVertexBufferSharedPtr vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
            kVertexSize, vertex_data->vertexCount, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        vertex_data->vertexBufferBinding->setBinding(0, vbuf);
        vbuf->writeData(0, vbuf->getSizeInBytes(), verts_.data(), true);

        const bool use32 = vertex_data->vertexCount > 0xFFFF;
        Ogre::HardwareIndexBufferSharedPtr ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
            use32 ? Ogre::HardwareIndexBuffer::IT_32BIT : Ogre::HardwareIndexBuffer::IT_16BIT,
            indices_.size(), Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        ibuf->writeData(0, ibuf->getSizeInBytes(), indices_.data(), true);

        sub->indexData->indexStart = 0;
        sub->indexData->indexCount = static_cast<unsigned>(indices_.size());
        sub->indexData->indexBuffer = ibuf;
        sub->useSharedVertices = true;
        mesh->sharedVertexData = vertex_data;

        // Bounds from the emitted vertices, so the mesh culls correctly.
        float min_x = 0.0f, min_y = 0.0f, max_x = 0.0f, max_y = 0.0f;
        for (size_t i = 0; i + 2 < verts_.size(); i += kFloatsPerVertex)
        {
            min_x = std::min(min_x, verts_[i]);
            min_y = std::min(min_y, verts_[i + 1]);
            max_x = std::max(max_x, verts_[i]);
            max_y = std::max(max_y, verts_[i + 1]);
        }
        const float max_extent = std::max({std::abs(min_x), std::abs(min_y), std::abs(max_x), std::abs(max_y)});
        mesh->_setBounds(Ogre::AxisAlignedBox(min_x, min_y, -0.01f, max_x, max_y, 0.01f));
        mesh->_setBoundingSphereRadius(max_extent);
        mesh->load();
        return mesh;
    }

    const std::vector<float>& vertices() const { return verts_; }

private:
    std::vector<float> verts_;
    std::vector<uint32_t> indices_;
};

/// The chibi core rig every race shares: an oversized faceted head, a tapered
/// torso, two arms and two legs. The character faces +X with the head toward
/// +Y, centred on the origin so the scene node can rotate it to face the
/// movement direction. `part` is the body colour; `shade` is a darker variant
/// for the far limbs.
void build_core_rig(MeshBuilder& mb, const MeshBuilder::Part& part, const MeshBuilder::Part& shade)
{
    // Head: a large faceted circle, the chibi silhouette's dominant feature.
    mb.fan(0.05f, 0.25f, 0.28f, 9, 0.0f, 0.0f, part);

    // Torso: a tapered quad, wider at the shoulders than the hips.
    mb.quad4({-0.13f, 0.17f, 0.11f, -0.10f}, {0.02f, 0.02f, -0.22f, -0.22f}, 0.0f, part);

    // Hips: a small quad joining torso and legs.
    mb.quad(-0.02f, -0.24f, 0.12f, 0.05f, 0.0f, 0.0f, part);

    // Arms: the near arm reaches forward and down, the far arm trails back.
    // A thin rotated quad reads as a limb at this scale.
    mb.quad(0.10f, -0.02f, 0.055f, 0.16f, -0.5f, 0.0f, part);   // near arm
    mb.quad(-0.08f, -0.04f, 0.05f, 0.15f, 0.55f, 0.0f, shade);  // far arm

    // Legs: short and slightly apart, with small feet.
    mb.quad(0.05f, -0.36f, 0.06f, 0.13f, 0.12f, 0.0f, part);    // near leg
    mb.quad(-0.07f, -0.36f, 0.055f, 0.13f, -0.10f, 0.0f, shade);  // far leg
    mb.quad(0.09f, -0.47f, 0.09f, 0.045f, 0.0f, 0.0f, part);    // near foot
    mb.quad(-0.05f, -0.47f, 0.085f, 0.045f, 0.0f, 0.0f, shade);   // far foot
}

/// The shared cape/scarf: a two-segment ribbon trailing from the shoulders
/// (the mesh's origin) back and down, so the cape node can rotate it to trail
/// behind the velocity vector. Built once and shared by every race.
Ogre::MeshPtr make_cape_mesh()
{
    MeshBuilder mb;
    const MeshBuilder::Part cloth{1.0f, 1.0f, 1.0f, 1.0f, 0.0f};

    // A ribbon from the shoulder (the origin) streaming back and down, with a
    // slight S-curve so it reads as cloth even at rest.
    const std::array<float, 4> top_x = {0.02f, -0.20f, -0.40f, -0.56f};
    const std::array<float, 4> top_y = {0.14f, 0.06f, -0.06f, -0.24f};
    const std::array<float, 4> bot_x = {0.02f, -0.22f, -0.44f, -0.64f};
    const std::array<float, 4> bot_y = {-0.04f, -0.12f, -0.24f, -0.44f};
    for (uint32_t i = 0; i < 3; ++i)
    {
        mb.quad4({top_x[i], bot_x[i], bot_x[i + 1], top_x[i + 1]},
                 {top_y[i], bot_y[i], bot_y[i + 1], top_y[i + 1]}, 0.0f, cloth);
    }

    return mb.bake("haven/race_cape");
}

}  // namespace

AnthropomorphicMeshBuilder::AnthropomorphicMeshBuilder(Ogre::SceneManager* scene_manager)
    : scene_(scene_manager)
{
}

Ogre::ColourValue AnthropomorphicMeshBuilder::race_body_colour(uint8_t race_id)
{
    return race_palette(race_id).body;
}

Ogre::ColourValue AnthropomorphicMeshBuilder::race_accent_colour(uint8_t race_id)
{
    return race_palette(race_id).accent;
}

Ogre::MeshPtr AnthropomorphicMeshBuilder::body_mesh_for_race(uint8_t race_id)
{
    auto it = bodies_.find(race_id);
    if (it != bodies_.end()) return it->second;
    Ogre::MeshPtr mesh = build_body_mesh(race_id);
    bodies_.emplace(race_id, mesh);
    return mesh;
}

Ogre::MeshPtr AnthropomorphicMeshBuilder::cape_mesh_for_race(uint8_t race_id)
{
    auto it = capes_.find(race_id);
    if (it != capes_.end()) return it->second;
    // One shared cape for every race; the variety is in the body appendages.
    static Ogre::MeshPtr shared_cape = make_cape_mesh();
    capes_.emplace(race_id, shared_cape);
    return shared_cape;
}

Ogre::MeshPtr AnthropomorphicMeshBuilder::build_body_mesh(uint8_t race_id)
{
    const Palette& pal = race_palette(race_id);
    const MeshBuilder::Part body{pal.body.r, pal.body.g, pal.body.b, 1.0f, 0.0f};
    // The far limbs and shaded undersides read as a darker shade of the body.
    const MeshBuilder::Part shade{body.r * 0.72f, body.g * 0.72f, body.b * 0.72f, 1.0f, 0.0f};
    const MeshBuilder::Part accent{pal.accent.r, pal.accent.g, pal.accent.b, 1.0f, 0.0f};
    const MeshBuilder::Part glow{pal.glow.r, pal.glow.g, pal.glow.b, 1.0f, 1.0f};

    MeshBuilder mb;

    // Translucent parts go first so the opaque body covers them: with alpha
    // blending and every part at the same z, the emission order is the draw
    // order, and a translucent wing drawn after the body would tint it.
    if (race_id == 1)  // Mantid Vex translucent wing-cases
    {
        const MeshBuilder::Part wing{pal.accent.r, pal.accent.g, pal.accent.b, 0.45f, 0.0f};
        mb.quad(-0.18f, 0.18f, 0.20f, 0.07f, 0.5f, 0.0f, wing);
        mb.quad(-0.18f, 0.02f, 0.20f, 0.07f, -0.5f, 0.0f, wing);
    }

    build_core_rig(mb, body, shade);

    switch (race_id)
    {
    case 0:  // Terran Kin: high-collared geometric visor, twin floating shoulder pylons.
    {
        // A high collar behind the head.
        mb.quad(-0.16f, 0.16f, 0.07f, 0.14f, 0.0f, 0.0f, shade);
        // The visor: a glowing band across the face.
        mb.quad(0.24f, 0.24f, 0.10f, 0.055f, 0.0f, 0.0f, glow);
        // Twin floating shoulder pylons: small diamonds either side.
        mb.fan(0.16f, 0.10f, 0.075f, 4, 0.0f, 0.0f, accent);
        mb.fan(-0.16f, 0.10f, 0.075f, 4, 0.0f, 0.0f, accent);
        break;
    }
    case 1:  // Mantid Vex: swept-back scythe forearms, V antennae (wings above).
    {
        // Scythe forearms: long swept blades replacing the near forearm.
        mb.tri(0.10f, 0.02f, 0.42f, -0.10f, 0.16f, -0.16f, 0.0f, 0.20f, -0.06f, 0.20f, accent);
        // V-shaped antennae: two thin blades from the head top.
        mb.tri(0.02f, 0.50f, 0.16f, 0.62f, 0.10f, 0.52f, 0.0f, 0.08f, 0.54f, 0.10f, glow);
        mb.tri(0.08f, 0.50f, -0.04f, 0.62f, 0.04f, 0.52f, 0.0f, 0.02f, 0.54f, 0.10f, glow);
        break;
    }
    case 2:  // Cephalon Tide: domed mantle crest, four curling lower tentacles.
    {
        // Domed mantle crest over the head.
        mb.fan(0.02f, 0.42f, 0.20f, 8, 0.0f, 0.0f, accent);
        // Four curling lower tentacles around the hips.
        mb.quad(-0.10f, -0.30f, 0.045f, 0.16f, 0.35f, 0.0f, accent);
        mb.quad(0.02f, -0.32f, 0.045f, 0.17f, 0.15f, 0.0f, accent);
        mb.quad(0.12f, -0.30f, 0.045f, 0.16f, -0.1f, 0.0f, accent);
        mb.quad(-0.20f, -0.28f, 0.04f, 0.15f, 0.55f, 0.0f, shade);
        break;
    }
    case 3:  // Aerie Seraph: layered chevron wing-blades, feathered tail fan.
    {
        // Layered chevron wing-blades: three blades fanning up and back.
        mb.tri(-0.05f, 0.10f, -0.42f, 0.42f, -0.30f, 0.16f, 0.0f, -0.20f, 0.24f, 0.28f, accent);
        mb.tri(-0.08f, 0.06f, -0.48f, 0.28f, -0.36f, 0.02f, 0.0f, -0.24f, 0.18f, 0.28f, body);
        mb.tri(-0.05f, 0.02f, -0.40f, 0.12f, -0.30f, -0.10f, 0.0f, -0.20f, 0.04f, 0.28f, accent);
        // Feathered tail fan below the hips.
        mb.fan(-0.18f, -0.30f, 0.16f, 6, 0.6f, 0.0f, shade);
        break;
    }
    case 4:  // Saurian Scute: dorsal ridge spikes, heavy pauldrons, segmented tail.
    {
        // Dorsal ridge spikes along the back.
        mb.tri(-0.16f, 0.10f, -0.30f, 0.22f, -0.22f, 0.02f, 0.0f, -0.24f, 0.10f, 0.16f, accent);
        mb.tri(-0.18f, -0.02f, -0.34f, 0.08f, -0.24f, -0.12f, 0.0f, -0.26f, -0.02f, 0.16f, accent);
        mb.tri(-0.16f, -0.14f, -0.30f, -0.06f, -0.22f, -0.22f, 0.0f, -0.24f, -0.14f, 0.16f, accent);
        // Heavy armoured pauldrons.
        mb.fan(0.14f, 0.08f, 0.12f, 6, 0.0f, 0.0f, shade);
        mb.fan(-0.14f, 0.08f, 0.12f, 6, 0.0f, 0.0f, shade);
        // Segmented tail behind the hips.
        mb.quad(-0.20f, -0.26f, 0.07f, 0.06f, 0.35f, 0.0f, accent);
        mb.quad(-0.30f, -0.30f, 0.055f, 0.05f, 0.45f, 0.0f, shade);
        break;
    }
    case 5:  // Abyssal Pelag: swept lateral head-fins, ribbon-like dorsal crest.
    {
        // Swept lateral head-fins.
        mb.tri(0.05f, 0.30f, -0.28f, 0.48f, 0.02f, 0.34f, 0.0f, -0.10f, 0.38f, 0.16f, accent);
        mb.tri(0.05f, 0.22f, -0.28f, 0.10f, 0.02f, 0.16f, 0.0f, -0.10f, 0.14f, 0.16f, accent);
        // Ribbon-like dorsal crest streaming back.
        mb.quad(-0.10f, 0.18f, 0.05f, 0.20f, 0.4f, 0.0f, glow);
        mb.quad(-0.16f, 0.02f, 0.045f, 0.16f, 0.55f, 0.0f, accent);
        break;
    }
    case 6:  // Weave Widow: four articulated dorsal spider-limbs, six glowing ocular gems.
    {
        // Four dorsal spider-limbs arcing up and back.
        mb.quad(-0.10f, 0.12f, 0.04f, 0.22f, 0.7f, 0.0f, shade);
        mb.quad(-0.14f, 0.02f, 0.04f, 0.24f, 0.9f, 0.0f, accent);
        mb.quad(-0.10f, -0.08f, 0.04f, 0.22f, -0.7f, 0.0f, shade);
        mb.quad(-0.06f, -0.16f, 0.035f, 0.20f, -0.9f, 0.0f, accent);
        // Six glowing ocular gems across the face.
        mb.fan(0.20f, 0.30f, 0.035f, 6, 0.0f, 0.0f, glow);
        mb.fan(0.24f, 0.24f, 0.035f, 6, 0.0f, 0.0f, glow);
        mb.fan(0.20f, 0.18f, 0.035f, 6, 0.0f, 0.0f, glow);
        mb.fan(0.13f, 0.33f, 0.03f, 6, 0.0f, 0.0f, glow);
        mb.fan(0.13f, 0.24f, 0.03f, 6, 0.0f, 0.0f, glow);
        mb.fan(0.13f, 0.15f, 0.03f, 6, 0.0f, 0.0f, glow);
        break;
    }
    case 7:  // Felis Nyx: tall triangular ears, whip-like S-curve tail.
    {
        // Tall triangular ears.
        mb.tri(-0.02f, 0.44f, 0.10f, 0.66f, 0.16f, 0.46f, 0.0f, 0.08f, 0.52f, 0.12f, body);
        mb.tri(0.10f, 0.44f, 0.24f, 0.62f, 0.18f, 0.46f, 0.0f, 0.16f, 0.52f, 0.12f, accent);
        // Whip-like S-curve tail: two offset quads behind the hips.
        mb.quad(-0.22f, -0.24f, 0.05f, 0.14f, 0.5f, 0.0f, accent);
        mb.quad(-0.34f, -0.18f, 0.045f, 0.13f, 0.75f, 0.0f, shade);
        break;
    }
    case 8:  // Canid Vanguard: lupine muzzle, upright pointed ears, jagged collar ruff.
    {
        // Lupine muzzle jutting forward.
        mb.quad4({0.24f, 0.38f, 0.34f, 0.22f}, {0.26f, 0.20f, 0.14f, 0.18f}, 0.0f, body);
        // Upright pointed ears.
        mb.tri(0.02f, 0.46f, 0.06f, 0.68f, 0.14f, 0.48f, 0.0f, 0.08f, 0.54f, 0.12f, body);
        mb.tri(0.14f, 0.46f, 0.20f, 0.64f, 0.26f, 0.48f, 0.0f, 0.20f, 0.54f, 0.12f, accent);
        // Jagged collar ruff around the neck.
        mb.tri(-0.10f, 0.12f, -0.26f, 0.20f, -0.16f, 0.02f, 0.0f, -0.16f, 0.08f, 0.16f, glow);
        mb.tri(-0.12f, 0.02f, -0.28f, -0.06f, -0.18f, -0.10f, 0.0f, -0.20f, -0.04f, 0.16f, glow);
        break;
    }
    default:  // 9: Cervid Sylvan: multi-branched crystalline antlers, hoofed lower legs.
    {
        // Multi-branched crystalline antlers.
        mb.tri(0.04f, 0.46f, 0.02f, 0.78f, 0.10f, 0.50f, 0.0f, 0.06f, 0.60f, 0.14f, glow);
        mb.tri(0.02f, 0.60f, -0.10f, 0.72f, 0.06f, 0.54f, 0.0f, -0.02f, 0.62f, 0.14f, glow);
        mb.tri(0.14f, 0.46f, 0.20f, 0.74f, 0.26f, 0.50f, 0.0f, 0.20f, 0.58f, 0.14f, glow);
        mb.tri(0.20f, 0.58f, 0.32f, 0.68f, 0.24f, 0.52f, 0.0f, 0.26f, 0.60f, 0.14f, glow);
        // Hoofed lower leg tapers.
        mb.quad4({0.02f, 0.10f, 0.09f, 0.01f}, {-0.40f, -0.40f, -0.50f, -0.50f}, 0.0f, shade);
        break;
    }
    }

    char name[64];
    std::snprintf(name, sizeof(name), "haven/race_body_%u", static_cast<unsigned>(race_id));
    return mb.bake(name);
}

}  // namespace haven
