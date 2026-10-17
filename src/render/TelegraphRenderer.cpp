#include "render/TelegraphRenderer.h"

#include <OgreMaterialManager.h>
#include <OgrePass.h>
#include <OgreResourceGroupManager.h>
#include <OgreRoot.h>
#include <OgreSceneManager.h>
#include <OgreTechnique.h>

#include <algorithm>
#include <cmath>

namespace haven {

namespace {

constexpr const char* kResourceGroup = "ShapedHaven";

/// How many enemies can be mid-windup at once. A swarm of darters all winding
/// up is the worst realistic case; beyond the pool the extras are simply not
/// painted, which reads as "no warning" rather than as a stale one.
constexpr size_t kMaxSlots = 64;

/// The shapes are arena-floor paint: above the grid and the gems, below the
/// enemy bodies they warn about, so a charge's rectangle runs under the
/// charger.
constexpr float kZTelegraph = 0.025f;

/// A CircleAoE's ring smoothness and a ConeSlash's arc smoothness. The shapes
/// are warnings, not art, so they stay cheap to rewrite every frame.
constexpr uint32_t kCircleSegments = 24;
constexpr uint32_t kConeSegments = 12;

/// The ConeSlash sector's arc. The wire carries no arc for the shape, so this
/// is the client's fixed reading of a cone attack: a quarter circle.
constexpr float kConeArc = 1.57079632679489661923f;  // pi / 2

/// Windup colour endpoints, lerped by telegraph_progress / 255: amber while
/// the attack is still far off, crimson as it lands.
constexpr float kAmberG = 0.667f;   // #FFAA00
constexpr float kCrimsonG = 0.067f; // #FF1133
constexpr float kCrimsonB = 0.20f;  // #33

/// The floor paint stays translucent so the arena grid reads through it.
constexpr float kAlpha = 0.55f;

Ogre::ColourValue lerp_colour(float t)
{
    return Ogre::ColourValue(1.0f, kAmberG + (kCrimsonG - kAmberG) * t, (kCrimsonB - 0.0f) * t, kAlpha);
}

}  // namespace

/// Uploads into a Manual Object through the same immediate-mode sink the
/// geometry builders write to, so the telegraph shapes and the tested builders
/// are one code path. Vertices carry no colour: the slot's material owns it.
class ManualPrimitiveSink : public PrimitiveSink {
public:
    explicit ManualPrimitiveSink(Ogre::ManualObject& object) : object_(object) {}

    uint32_t base_index() const override { return vertex_count_; }

    void vertex(float x, float y, float z) override
    {
        object_.position(x, y, z);
        object_.normal(0.0f, 0.0f, 1.0f);
        ++vertex_count_;
    }

    void index(uint32_t index) override { object_.index(index); }

private:
    Ogre::ManualObject& object_;
    uint32_t vertex_count_ = 0;
};

TelegraphRenderer::TelegraphRenderer(Ogre::SceneManager* scene_manager) : scene_(scene_manager)
{
    if (scene_ == nullptr) return;

    // One Manual Object per slot, created up front so the render loop never
    // builds scene objects. Each begins with a dummy triangle: a Manual
    // Object with no vertices discards its section, and beginUpdate() on a
    // discarded section is undefined.
    for (size_t i = 0; i < kMaxSlots; ++i)
    {
        Ogre::ManualObject* object = scene_->createManualObject("haven/telegraph_" + std::to_string(i));
        object->setDynamic(true);
        object->estimateVertexCount(64);
        object->estimateIndexCount(96);

        Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().create(
            "haven/telegraph_colour_" + std::to_string(i), kResourceGroup);
        Ogre::Pass* pass = material->getTechnique(0)->getPass(0);
        pass->setAmbient(Ogre::ColourValue(1.0f, kAmberG, 0.0f, kAlpha));
        pass->setDiffuse(Ogre::ColourValue(1.0f, kAmberG, 0.0f, kAlpha));
        pass->setSelfIllumination(Ogre::ColourValue(1.0f, kAmberG, 0.0f, kAlpha));
        pass->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
        material->load();

        object->begin(material->getName(), Ogre::RenderOperation::OT_TRIANGLE_LIST, kResourceGroup);
        for (uint32_t v = 0; v < 3; ++v)
        {
            object->position(0.0f, 0.0f, 0.0f);
            object->normal(0.0f, 0.0f, 1.0f);
            object->index(v);
        }
        object->end();
        // The shapes stay inside the arena, so a static arena-wide bound needs
        // no per-frame update; the objects are culled against it.
        object->setBoundingBox(Ogre::AxisAlignedBox(-kArenaHalfSize, -kArenaHalfSize, -1.0f, kArenaHalfSize,
                                                    kArenaHalfSize, 1.0f));
        scene_->getRootSceneNode()
            ->createChildSceneNode("haven/telegraph_node_" + std::to_string(i))
            ->attachObject(object);

        Slot slot;
        slot.object = object;
        slot.material = material;
        slot.in_use = false;
        slots_.push_back(slot);
    }
    initialised_ = true;
}

TelegraphRenderer::~TelegraphRenderer()
{
    shutdown();
}

void TelegraphRenderer::layout_slot(size_t index, const EntityDrawItem& item)
{
    Slot& slot = slots_[index];
    slot.in_use = true;

    const float t = static_cast<float>(item.telegraph_progress) / 255.0f;
    const Ogre::ColourValue paint = lerp_colour(t);
    Ogre::Pass* pass = slot.material->getTechnique(0)->getPass(0);
    pass->setAmbient(paint);
    pass->setDiffuse(paint);
    pass->setSelfIllumination(paint);

    // The wire's angle is milliradians; the geometry builders take radians.
    const float angle = static_cast<float>(item.telegraph_angle) / 1000.0f;
    const float radius = static_cast<float>(item.telegraph_radius);

    Ogre::ManualObject* object = slot.object;
    object->beginUpdate(0);
    ManualPrimitiveSink sink(*object);
    switch (item.telegraph_shape)
    {
    case 1:  // CircleAoE: the blast is centred on the enemy.
        append_circle(sink, item.pos, radius, kZTelegraph, kCircleSegments);
        break;
    case 2:  // ConeSlash: a sector opening along the facing direction.
        append_arc(sink, item.pos, radius, angle, kConeArc, kZTelegraph, kConeSegments);
        break;
    default:  // LineCharge, and any shape this client does not know: the path
              // the enemy will travel, from its centre out to the charge's
              // reach, as wide as its body.
        if (radius > 0.0f)
        {
            const float half_width = enemy_radius(item.style);
            const Vec2 direction{std::cos(angle), std::sin(angle)};
            const Vec2 centre = item.pos + direction * (radius * 0.5f);
            append_quad(sink, centre, Vec2{radius * 0.5f, half_width}, angle, kZTelegraph);
        }
        break;
    }
    object->end();
}

void TelegraphRenderer::hide_slot(size_t index)
{
    slots_[index].in_use = false;
    slots_[index].object->setVisible(false);
}

void TelegraphRenderer::render(const std::vector<EntityDrawItem>& items)
{
    if (!initialised_) return;

    for (Slot& slot : slots_) slot.in_use = false;

    size_t used = 0;
    for (const EntityDrawItem& item : items)
    {
        if (item.kind != EntityKind::Enemy) continue;
        if (item.telegraph_progress == 0) continue;
        if (item.telegraph_shape == 0) continue;  // None: no attack is coming
        if (used >= slots_.size()) break;         // pool full: paint the rest not
        layout_slot(used++, item);
    }

    for (size_t i = used; i < slots_.size(); ++i)
    {
        Slot& slot = slots_[i];
        if (!slot.in_use) continue;
        // One frame of an empty section: a slot left alone keeps drawing last
        // frame's geometry, and a windup that ended must disappear the moment
        // its enemy's progress returns to zero.
        slot.object->beginUpdate(0);
        slot.object->end();
        slot.object->setVisible(false);
    }

    for (size_t i = 0; i < used; ++i) slots_[i].object->setVisible(true);
}

void TelegraphRenderer::shutdown()
{
    if (!initialised_) return;
    initialised_ = false;
    for (Slot& slot : slots_)
    {
        if (slot.object == nullptr) continue;
        if (slot.object->getParentSceneNode() != nullptr)
            scene_->destroySceneNode(slot.object->getParentSceneNode());
        slot.object = nullptr;
        slot.material.setNull();
        slot.in_use = false;
    }
    slots_.clear();
}

}  // namespace haven
