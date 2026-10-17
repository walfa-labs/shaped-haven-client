#include "hud.h"

#include "character_catalog.h"
#include "hud_model.h"
#include "levelup_format.h"
#include "hud_text.h"
#include "mesh_factory.h"

#include <OgreAxisAlignedBox.h>
#include <OgreEntity.h>
#include <OgreMaterialManager.h>
#include <OgrePass.h>
#include <OgreResourceGroupManager.h>
#include <OgreRoot.h>
#include <OgreSceneManager.h>
#include <OgreTechnique.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace haven {

namespace {

constexpr const char* kResourceGroup = "ShapedHaven";
constexpr const char* kQuadMesh = "haven/player_square";

/// DESIGN.md section 1: the orthographic viewport covers 80 x 45 units.
constexpr float kViewportHalfWidth = 40.0f;
constexpr float kViewportHalfHeight = 22.5f;

/// Bar geometry, in viewport units.
constexpr float kXpBarX = -36.0f;
constexpr float kXpBarWidth = 72.0f;
constexpr float kXpBarY = 19.1f;  // centre line
constexpr float kXpBarHeight = 1.4f;

/// Item tray geometry.
constexpr float kTrayWeaponWidth = 6.4f;
constexpr float kTrayWeaponHeight = 3.2f;
constexpr float kTrayPassiveWidth = 6.4f;
constexpr float kTrayPassiveHeight = 2.6f;
constexpr float kTrayStep = 6.6f;
constexpr float kTrayWeaponY = -20.3f;  // centre line of the weapon row
constexpr float kTrayPassiveY0 = -16.1f;
constexpr float kTrayPassiveY1 = -12.9f;

/// Modal geometry: three cards across the middle of the screen.
constexpr float kCardWidth = 20.0f;
constexpr float kCardHeight = 14.5f;
constexpr float kCardY = 0.0f;
constexpr float kCardX[3] = {-21.5f, 0.0f, 21.5f};

constexpr float kPipSize = 0.30f;
constexpr float kPipSpacing = 0.44f;

/// Character select geometry. The screen is one panel over the whole viewport:
/// the name field across the top, the ten races down the left, the chosen
/// race's stats and the four weapons on the right, and START at the bottom.
constexpr int kSelectFieldName = 0;
constexpr int kSelectFieldRace = 1;
constexpr int kSelectFieldWeapon = 2;
constexpr int kSelectFieldStart = 3;

constexpr float kSelectNameY = 16.0f;     // centre line of the name field
constexpr float kSelectNameX = -25.0f;    // left edge of the name's value
constexpr float kSelectNamePixel = 0.40f;
constexpr float kSelectRowLeft = -34.0f;  // left edge of the race list
constexpr float kSelectRaceTop = 13.5f;   // top edge of the first race row
constexpr float kSelectRaceStep = 1.75f;
constexpr float kSelectRacePixel = 0.24f;
/// The ten rows as one block, derived from the rows themselves so the box
/// always covers them.
constexpr float kSelectRaceBottom = kSelectRaceTop - 9.0f * kSelectRaceStep - 7.0f * kSelectRacePixel;
constexpr float kSelectRaceBoxY = (kSelectRaceTop + kSelectRaceBottom) * 0.5f;
constexpr float kSelectRaceBoxHalfH = (kSelectRaceTop - kSelectRaceBottom) * 0.5f + 0.5f;
constexpr float kSelectDetailX = 1.0f;    // left edge of the right column
constexpr float kSelectWeaponY = -8.5f;   // centre line of the weapon row
constexpr float kSelectWeaponHalfW = 8.2f;
constexpr float kSelectWeaponHalfH = 2.5f;
constexpr float kSelectWeaponX[4] = {-27.0f, -9.0f, 9.0f, 27.0f};
constexpr float kSelectStartY = -16.0f;   // centre line of the START button

// The blocks run top to bottom, so each has to clear the one below it: an
// overlap draws the last race names over the weapon row.
static_assert(kSelectNameY - 1.5f > kSelectRaceTop, "the name field runs into the race list");
static_assert(kSelectRaceBoxY + kSelectRaceBoxHalfH < kSelectNameY - 1.5f, "the race box runs into the name field");
static_assert(kSelectRaceBottom > kSelectWeaponY + kSelectWeaponHalfH,
              "the race list runs into the weapon row");
static_assert(kSelectWeaponY - kSelectWeaponHalfH > kSelectStartY + 1.5f, "the weapon row runs into START");
static_assert(kSelectStartY - 1.5f > -23.0f, "START falls off the bottom of the screen");

/// Uploads into a ManualObject through the same immediate-mode sink the
/// geometry builders write to, so HUD text and the tested builders are one
/// code path. Vertices carry no colour: the material owns it.
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

    void reset() { vertex_count_ = 0; }

private:
    Ogre::ManualObject& object_;
    uint32_t vertex_count_ = 0;
};

std::string format_time(uint32_t run_time_ms)
{
    const uint32_t total_seconds = run_time_ms / 1000u;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02u:%02u", static_cast<unsigned>(total_seconds / 60u),
                  static_cast<unsigned>(total_seconds % 60u));
    return buf;
}

/// Splits a name into at most two display lines of `max_chars` characters,
/// preferring a break at a space.
std::vector<std::string> split_name(const std::string& name, size_t max_chars)
{
    std::vector<std::string> lines;
    std::string remaining = name;
    while (remaining.size() > max_chars)
    {
        const size_t space = remaining.rfind(' ', max_chars);
        if (space == std::string::npos || space == 0)
        {
            lines.push_back(remaining.substr(0, max_chars));
            remaining = remaining.substr(max_chars);
        }
        else
        {
            lines.push_back(remaining.substr(0, space));
            remaining = remaining.substr(space + 1);
        }
    }
    lines.push_back(remaining);
    return lines;
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction and the persistent full-screen quads.
// ---------------------------------------------------------------------------

Hud::Hud(Ogre::SceneManager* scene_manager, MeshFactory* meshes, Ogre::SceneNode* camera_node)
    : scene_(scene_manager), meshes_(meshes), camera_node_(camera_node)
{
    tray_.name = "haven/hud_tray";
    modal_.name = "haven/hud_modal";
    menu_.name = "haven/hud_menu";
    select_fields_.name = "haven/hud_select_fields";
    select_fields_.z = 0.488f;
    select_.name = "haven/hud_select";
    select_.z = 0.490f;
    attach_node_ = camera_node_->createChildSceneNode();
    attach_node_->setPosition(0.0f, 0.0f, -kCameraZ);
    build_persistent_elements();
}

Hud::~Hud()
{
    destroy_layer(hud_lines_);
    destroy_layer(tray_text_);
    destroy_layer(modal_title_);
    for (TextLayer*& layer : modal_cards_)
        destroy_layer(layer);
    for (TextLayer*& layer : modal_stats_)
        destroy_layer(layer);
    destroy_layer(modal_footer_);
    destroy_layer(score_title_);
    destroy_layer(score_lines_);
    destroy_layer(score_footer_);
    destroy_layer(banner_tag_);
    destroy_layer(banner_name_);
    destroy_layer(menu_title_);
    destroy_layer(menu_options_);
    destroy_layer(menu_footer_);
    destroy_layer(menu_error_);
    destroy_layer(select_title_);
    destroy_layer(select_name_);
    destroy_layer(select_races_);
    destroy_layer(select_detail_);
    destroy_layer(select_weapons_);
    destroy_layer(select_start_);
    destroy_layer(select_hints_);
}

Ogre::MaterialPtr Hud::material_for(Rgba colour)
{
    struct ColourLess {
        bool operator()(const std::array<float, 4>& a, const std::array<float, 4>& b) const { return a < b; }
    };
    const std::array<float, 4> key{colour.r, colour.g, colour.b, colour.a};
    const auto it = material_cache_.find(key);
    if (it != material_cache_.end()) return it->second;

    // The name is the colour, not a sequence number: two screens that reach
    // the same colour in a different order would otherwise swap names, and
    // the shader system keys what it generates on the material name.
    uint64_t hash = 1469598103934665603ull;
    for (const float channel : key)
    {
        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&channel);
        for (size_t i = 0; i < sizeof(float); ++i)
        {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
    }
    char name[48];
    std::snprintf(name, sizeof(name), "haven/hud_colour_%016llx", static_cast<unsigned long long>(hash));
    Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().create(name, kResourceGroup);
    Ogre::Pass* pass = material->getTechnique(0)->getPass(0);
    pass->setAmbient(Ogre::ColourValue(colour.r, colour.g, colour.b, colour.a));
    pass->setDiffuse(Ogre::ColourValue(colour.r, colour.g, colour.b, colour.a));
    pass->setSelfIllumination(Ogre::ColourValue(colour.r, colour.g, colour.b, colour.a));
    if (colour.a < 0.999f) pass->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
    material->load();

    material_cache_.emplace(key, material);
    return material;
}

void Hud::build_persistent_elements()
{
    auto make_quad = [this](const char* name, Rgba colour, float z) {
        Ogre::Entity* entity = scene_->createEntity(name, kQuadMesh, kResourceGroup);
        entity->setMaterialName(material_for(colour)->getName(), kResourceGroup);
        // The HUD lives in camera-local space: the camera node translates
        // with the player, so a quad at local (0, 0, z) is always centred.
        Ogre::SceneNode* node = attach_node_->createChildSceneNode();
        node->attachObject(entity);
        node->setPosition(0.0f, 0.0f, z);
        entity->setVisible(false);
        return entity;
    };

    xp_bg_ = make_quad("haven/xp_bar_bg", Rgba{0.07f, 0.08f, 0.12f, 0.92f}, 0.06f);
    xp_fill_ = make_quad("haven/xp_bar_fill", Rgba{0.35f, 0.85f, 0.55f, 0.95f}, 0.07f);
    dim_ = make_quad("haven/hud_dim", Rgba{0.01f, 0.01f, 0.02f, 0.72f}, 0.48f);
    panel_ = make_quad("haven/hud_panel", Rgba{0.06f, 0.07f, 0.11f, 0.90f}, 0.485f);

    // The quad mesh is 1x1 unit; the persistent elements are sized once here.
    xp_bg_->getParentNode()->setScale(kXpBarWidth, kXpBarHeight, 1.0f);
    xp_bg_->getParentNode()->setPosition(kXpBarX + kXpBarWidth * 0.5f, kXpBarY, 0.06f);
    dim_->getParentNode()->setScale(82.0f, 47.0f, 1.0f);
    panel_->getParentNode()->setScale(48.0f, 24.0f, 1.0f);

    xp_bg_->setVisible(true);
    initialised_ = true;
}

// ---------------------------------------------------------------------------
// Per-frame state.
// ---------------------------------------------------------------------------

void Hud::set_run_time(uint32_t run_time_ms)
{
    if (!initialised_) return;

    // `loadout_` is empty until the first S2C_LOADOUT arrives, and again after
    // a restart clears the HUD; GameSession::update() calls this every frame,
    // so the empty case has to be answered without reading through it.
    const XpBarFill fill = xp_bar_fill(loadout_);
    if (fill.visible)
    {
        const float width = kXpBarWidth * fill.fraction;
        if (width > 0.01f)
        {
            xp_fill_->setVisible(true);
            Ogre::Node* node = xp_fill_->getParentNode();
            node->setScale(width, kXpBarHeight, 1.0f);
            node->setPosition(kXpBarX + width * 0.5f, kXpBarY, 0.07f);
        }
        else
        {
            xp_fill_->setVisible(false);
        }
    }
    else
    {
        xp_fill_->setVisible(false);
    }

    const uint32_t second = run_time_ms / 1000u;
    if (second == last_run_second_) return;
    last_run_second_ = second;

    // The run clock is the only per-second HUD change.
    if (hud_lines_ != nullptr && !hud_lines_->lines.empty())
    {
        hud_lines_->lines.back().text = "TIME " + format_time(run_time_ms);
        rebuild_layer(*hud_lines_);
    }
}

void Hud::set_loadout(const Loadout& loadout)
{
    if (!initialised_) return;
    loadout_ = loadout;
    layout_tray(loadout);
    rebuild_hud_lines();
}

void Hud::show_offer(const LevelUpOffer& offer)
{
    if (!initialised_) return;
    layout_offer(offer);
    offer_visible_ = true;
    dim_->setVisible(true);
    panel_->setVisible(true);
    Ogre::Node* panel_node = panel_->getParentNode();
    panel_node->setScale(66.0f, 18.5f, 1.0f);
    panel_node->setPosition(0.0f, kCardY - 0.5f, 0.485f);
}

void Hud::hide_offer()
{
    if (!initialised_) return;
    clear_batch(modal_);
    destroy_layer(modal_title_);
    for (TextLayer*& layer : modal_cards_)
        destroy_layer(layer);
    for (TextLayer*& layer : modal_stats_)
        destroy_layer(layer);
    destroy_layer(modal_footer_);
    offer_visible_ = false;
    dim_->setVisible(false);
    panel_->setVisible(false);
}

void Hud::show_evolution(uint8_t evo_item_id)
{
    if (!initialised_) return;
    layout_banner(item_name(evo_item_id));
    banner_visible_ = true;
    banner_timer_ = kBannerSeconds;
}

void Hud::show_game_over(const GameOverStats& stats)
{
    if (!initialised_) return;
    layout_game_over(stats);
    game_over_visible_ = true;
    dim_->setVisible(true);
    panel_->setVisible(true);
    Ogre::Node* panel_node = panel_->getParentNode();
    panel_node->setScale(48.0f, 24.0f, 1.0f);
    panel_node->setPosition(0.0f, 0.0f, 0.485f);
}

void Hud::hide_game_over()
{
    if (!initialised_) return;
    destroy_layer(score_title_);
    destroy_layer(score_lines_);
    destroy_layer(score_footer_);
    game_over_visible_ = false;
    dim_->setVisible(false);
    panel_->setVisible(false);
}

void Hud::reset()
{
    if (!initialised_) return;
    loadout_.reset();
    hide_offer();
    hide_game_over();
    xp_fill_->setVisible(false);
    clear_batch(tray_);
    destroy_layer(tray_text_);
    destroy_layer(hud_lines_);
    last_run_second_ = 0xFFFFFFFFu;
    banner_visible_ = false;
    banner_timer_ = 0.0f;
}

// ---------------------------------------------------------------------------
// Main menu.
// ---------------------------------------------------------------------------

void Hud::show_main_menu()
{
    if (!initialised_) return;
    layout_main_menu();
    menu_visible_ = true;
    // The XP bar is run-bound; it comes back when the menu goes away.
    xp_bg_->setVisible(false);
    xp_fill_->setVisible(false);
    dim_->setVisible(true);
    panel_->setVisible(true);
    Ogre::Node* panel_node = panel_->getParentNode();
    panel_node->setScale(46.0f, 33.0f, 1.0f);
    panel_node->setPosition(0.0f, 0.0f, 0.485f);
}

void Hud::hide_main_menu()
{
    if (!initialised_) return;
    clear_batch(menu_);
    destroy_layer(menu_title_);
    destroy_layer(menu_options_);
    destroy_layer(menu_footer_);
    destroy_layer(menu_error_);
    menu_visible_ = false;
    xp_bg_->setVisible(true);
    dim_->setVisible(false);
    panel_->setVisible(false);
}

void Hud::set_menu_selection(int selection)
{
    if (!initialised_) return;
    menu_selection_ = selection;
    if (menu_visible_) layout_main_menu();
}

void Hud::set_menu_error(const std::string& error)
{
    if (!initialised_) return;
    menu_error_text_ = error;
    if (menu_visible_) layout_main_menu();
}

void Hud::clear_menu_error()
{
    if (!initialised_) return;
    menu_error_text_.clear();
    if (menu_visible_) layout_main_menu();
}

// ---------------------------------------------------------------------------
// Character select.
// ---------------------------------------------------------------------------

void Hud::show_character_select()
{
    if (!initialised_) return;
    layout_character_select();
    select_visible_ = true;
    // The XP bar is run-bound; it comes back when the screen goes away.
    xp_bg_->setVisible(false);
    xp_fill_->setVisible(false);
    dim_->setVisible(true);
    panel_->setVisible(true);
    Ogre::Node* panel_node = panel_->getParentNode();
    panel_node->setScale(82.0f, 46.0f, 1.0f);
    panel_node->setPosition(0.0f, 0.0f, 0.485f);
}

void Hud::hide_character_select()
{
    if (!initialised_) return;
    clear_batch(select_);
    clear_batch(select_fields_);
    destroy_layer(select_title_);
    destroy_layer(select_name_);
    destroy_layer(select_races_);
    destroy_layer(select_detail_);
    destroy_layer(select_weapons_);
    destroy_layer(select_start_);
    destroy_layer(select_hints_);
    select_visible_ = false;
    dim_->setVisible(false);
    panel_->setVisible(false);
}

void Hud::set_character_select_selection(int selection)
{
    if (!initialised_ || selection == select_selection_) return;
    select_selection_ = selection;
    if (select_visible_) layout_character_select();
}

void Hud::set_character_select_name(const std::string& name)
{
    if (!initialised_ || name == select_name_text_) return;
    select_name_text_ = name;
    if (select_visible_) layout_character_select();
}

void Hud::set_character_select_race(int race_id)
{
    if (!initialised_ || race_id == select_race_) return;
    select_race_ = race_id;
    if (select_visible_) layout_character_select();
}

void Hud::set_character_select_weapon(int weapon_id)
{
    if (!initialised_ || weapon_id == select_weapon_) return;
    select_weapon_ = weapon_id;
    if (select_visible_) layout_character_select();
}

void Hud::shutdown()
{
    if (!initialised_) return;
    loadout_.reset();
    hide_offer();
    hide_game_over();
    hide_main_menu();
    hide_character_select();
    clear_batch(tray_);
    destroy_layer(tray_text_);
    destroy_layer(hud_lines_);
    destroy_layer(banner_tag_);
    destroy_layer(banner_name_);
    banner_visible_ = false;
    material_cache_.clear();
    initialised_ = false;
}

void Hud::tick(float dt_seconds)
{
    if (!banner_visible_) return;
    banner_timer_ -= dt_seconds;
    if (banner_timer_ <= 0.0f)
    {
        banner_visible_ = false;
        destroy_layer(banner_tag_);
        destroy_layer(banner_name_);
    }
}

// ---------------------------------------------------------------------------
// Text layers.
// ---------------------------------------------------------------------------

Hud::TextLayer* Hud::ensure_layer(TextLayer*& ptr)
{
    if (ptr == nullptr)
    {
        ptr = new TextLayer();
        ptr->name = "haven/hud_text_" + std::to_string(reinterpret_cast<uintptr_t>(ptr));
    }
    return ptr;
}

void Hud::rebuild_layer(TextLayer& layer)
{
    if (layer.object != nullptr)
    {
        scene_->destroyManualObject(layer.object);
        layer.object = nullptr;
    }
    if (layer.lines.empty()) return;

    if (layer.node == nullptr)
    {
        layer.node = attach_node_->createChildSceneNode();
        layer.node->setPosition(0.0f, 0.0f, 0.0f);
    }
    if (layer.object != nullptr)
    {
        scene_->destroyManualObject(layer.object);
        layer.object = nullptr;
    }

    Ogre::ManualObject* object = scene_->createManualObject(layer.name);
    object->setDynamic(true);
    layer.node->attachObject(object);
    ManualPrimitiveSink sink(*object);

    for (const TextLine& line : layer.lines)
    {
        const uint32_t lit = static_cast<uint32_t>(text_lit_pixels(line.text));
        object->estimateVertexCount(lit * 4u);
        object->estimateIndexCount(lit * 6u);
        object->begin(material_for(line.colour)->getName(), Ogre::RenderOperation::OT_TRIANGLE_LIST,
                      kResourceGroup);
        sink.reset();
        append_text(sink, line.text, Vec2{line.x, line.y}, line.pixel);
        object->end();
    }

    object->setBoundingBox(Ogre::AxisAlignedBox(-kViewportHalfWidth, -kViewportHalfHeight, 0.4f,
                                                kViewportHalfWidth, kViewportHalfHeight, 0.6f));
    layer.object = object;
}

void Hud::clear_layer(TextLayer& layer)
{
    if (layer.object != nullptr)
    {
        scene_->destroyManualObject(layer.object);
        layer.object = nullptr;
    }
    layer.lines.clear();
}

void Hud::destroy_layer(TextLayer*& ptr)
{
    if (ptr == nullptr) return;
    clear_layer(*ptr);
    if (ptr->node != nullptr)
    {
        scene_->destroySceneNode(ptr->node);
        ptr->node = nullptr;
    }
    delete ptr;
    ptr = nullptr;
}

// ---------------------------------------------------------------------------
// Quad batch (tray boxes, modal cards).
// ---------------------------------------------------------------------------

void Hud::quad_into(QuadBatch& batch, Rgba colour, float cx, float cy, float hx, float hy)
{
    QuadBatch::Section* section = nullptr;
    for (QuadBatch::Section& existing : batch.sections)
    {
        if (existing.colour.r == colour.r && existing.colour.g == colour.g &&
            existing.colour.b == colour.b && existing.colour.a == colour.a)
        {
            section = &existing;
            break;
        }
    }
    if (section == nullptr)
    {
        batch.sections.push_back(QuadBatch::Section{});
        section = &batch.sections.back();
        section->colour = colour;
    }
    // Same counter-clockwise winding as PrimitiveSink::quad.
    const std::array<std::pair<float, float>, 4> corners{{{-hx, -hy}, {hx, -hy}, {hx, hy}, {-hx, hy}}};
    for (const auto& corner : corners)
    {
        section->xs.push_back(cx + corner.first);
        section->ys.push_back(cy + corner.second);
    }
    section->verts += 4;
    section->indices += 6;
}

void Hud::rebuild_batch(QuadBatch& batch)
{
    if (batch.object != nullptr)
    {
        scene_->destroyManualObject(batch.object);
        batch.object = nullptr;
    }
    if (batch.sections.empty()) return;

    if (batch.node == nullptr)
    {
        batch.node = attach_node_->createChildSceneNode();
        batch.node->setPosition(0.0f, 0.0f, 0.0f);
    }
    if (batch.object != nullptr)
    {
        scene_->destroyManualObject(batch.object);
        batch.object = nullptr;
    }

    Ogre::ManualObject* object = scene_->createManualObject(batch.name);
    object->setDynamic(true);
    batch.node->attachObject(object);

    for (const QuadBatch::Section& section : batch.sections)
    {
        const Ogre::String material_name = material_for(section.colour)->getName();
        object->estimateVertexCount(section.verts);
        object->estimateIndexCount(section.indices);
        object->begin(material_name, Ogre::RenderOperation::OT_TRIANGLE_LIST, kResourceGroup);
        for (size_t quad = 0; quad < section.xs.size(); quad += 4)
        {
            for (int corner = 0; corner < 4; ++corner)
            {
                object->position(section.xs[quad + corner], section.ys[quad + corner], batch.z);
                object->normal(0.0f, 0.0f, 1.0f);
            }
            const uint32_t base = static_cast<uint32_t>(quad);
            object->index(base + 0);
            object->index(base + 1);
            object->index(base + 2);
            object->index(base + 0);
            object->index(base + 2);
            object->index(base + 3);
        }
        object->end();
    }

    object->setBoundingBox(Ogre::AxisAlignedBox(-kViewportHalfWidth, -kViewportHalfHeight, 0.4f,
                                                kViewportHalfWidth, kViewportHalfHeight, 0.6f));
    batch.object = object;
}

void Hud::clear_batch(QuadBatch& batch)
{
    if (batch.object != nullptr)
    {
        scene_->destroyManualObject(batch.object);
        batch.object = nullptr;
    }
    batch.sections.clear();
}

// ---------------------------------------------------------------------------
// Layouts.
// ---------------------------------------------------------------------------

void Hud::layout_tray(const Loadout& loadout)
{
    clear_batch(tray_);

    const Rgba kInner{0.05f, 0.06f, 0.09f, 0.85f};
    const Rgba kPipEmpty{0.15f, 0.16f, 0.20f, 0.9f};
    constexpr float kBorder = 0.25f;

    auto box = [&](Rgba colour, float x, float y, float w, float h) {
        quad_into(tray_, colour, x, y, w * 0.5f, h * 0.5f);
        quad_into(tray_, kInner, x, y, (w - 2.0f * kBorder) * 0.5f, (h - 2.0f * kBorder) * 0.5f);
    };

    for (size_t i = 0; i < 4; ++i)
    {
        const float x = -36.5f + static_cast<float>(i) * kTrayStep;
        if (i < loadout.weapon_count)
            box(item_colour(loadout.weapons[i].item_id), x, kTrayWeaponY, kTrayWeaponWidth,
                kTrayWeaponHeight);
        else
            box(kInner, x, kTrayWeaponY, kTrayWeaponWidth, kTrayWeaponHeight);
    }

    for (size_t i = 0; i < 10; ++i)
    {
        const float x = -36.5f + static_cast<float>(i % 5) * kTrayStep;
        const float y = i < 5 ? kTrayPassiveY0 : kTrayPassiveY1;
        if (i < loadout.passive_count)
            box(item_colour(loadout.passives[i].item_id), x, y, kTrayPassiveWidth,
                kTrayPassiveHeight);
        else
            box(kInner, x, y, kTrayPassiveWidth, kTrayPassiveHeight);
    }

    // Level pips, drawn into the same batch: filled in the item's colour,
    // empty in dark grey, one per level of the slot's max.
    auto pips = [&](Rgba colour, uint8_t max_level, uint8_t level, float x, float y) {
        const uint8_t shown = level > max_level ? max_level : level;
        for (uint8_t pip = 0; pip < max_level; ++pip)
        {
            const float offset = (static_cast<float>(pip) - static_cast<float>(max_level - 1) * 0.5f) *
                                 kPipSpacing;
            quad_into(tray_, pip < shown ? colour : kPipEmpty, x + offset, y, kPipSize * 0.5f,
                      kPipSize * 0.5f);
        }
    };

    for (size_t i = 0; i < loadout.weapon_count && i < 4; ++i)
    {
        const LoadoutSlot& slot = loadout.weapons[i];
        if (slot.evolved) continue;  // evolved weapons show a tag, not pips
        pips(item_colour(slot.item_id), item_max_level(slot.item_id), slot.level,
             -36.5f + static_cast<float>(i) * kTrayStep, kTrayWeaponY - 1.05f);
    }
    for (size_t i = 0; i < loadout.passive_count && i < 10; ++i)
    {
        const LoadoutSlot& slot = loadout.passives[i];
        pips(item_colour(slot.item_id), item_max_level(slot.item_id), slot.level,
             -36.5f + static_cast<float>(i % 5) * kTrayStep,
             (i < 5 ? kTrayPassiveY0 : kTrayPassiveY1) - 0.55f);
    }

    rebuild_batch(tray_);

    // One text layer for every item name and tag on the tray.
    TextLayer* text = ensure_layer(tray_text_);
    text->lines.clear();

    for (size_t i = 0; i < loadout.weapon_count && i < 4; ++i)
    {
        const LoadoutSlot& slot = loadout.weapons[i];
        const float x = -36.5f + static_cast<float>(i) * kTrayStep;
        TextLine name;
        name.text = item_short_name(slot.item_id);
        name.pixel = fit_text_pixel(name.text, kTrayWeaponWidth - 1.0f, 0.0f, 0.30f);
        name.x = x - static_cast<float>(text_pixel_width(name.text)) * name.pixel * 0.5f;
        name.y = kTrayWeaponY + 0.55f;
        name.colour = Rgba{0.92f, 0.94f, 0.98f, 1.0f};
        text->lines.push_back(name);

        if (slot.evolved)
        {
            TextLine tag;
            tag.text = "EVOLVED";
            tag.pixel = 0.22f;
            tag.x = x - static_cast<float>(text_pixel_width(tag.text)) * tag.pixel * 0.5f;
            tag.y = kTrayWeaponY - 1.30f;
            tag.colour = Rgba{1.00f, 0.80f, 0.30f, 1.0f};
            text->lines.push_back(tag);
        }
    }

    for (size_t i = 0; i < loadout.passive_count && i < 10; ++i)
    {
        const LoadoutSlot& slot = loadout.passives[i];
        const float x = -36.5f + static_cast<float>(i % 5) * kTrayStep;
        TextLine name;
        name.text = item_short_name(slot.item_id);
        name.pixel = fit_text_pixel(name.text, kTrayPassiveWidth - 1.0f, 0.0f, 0.20f);
        name.x = x - static_cast<float>(text_pixel_width(name.text)) * name.pixel * 0.5f;
        name.y = (i < 5 ? kTrayPassiveY0 : kTrayPassiveY1) + 0.45f;
        name.colour = Rgba{0.92f, 0.94f, 0.98f, 1.0f};
        text->lines.push_back(name);
    }

    rebuild_layer(*text);
}

void Hud::rebuild_hud_lines()
{
    TextLayer* layer = ensure_layer(hud_lines_);
    layer->lines.clear();

    const Loadout& loadout = loadout_.has_value() ? *loadout_ : Loadout{};

    TextLine level;
    level.text = loadout_.has_value() ? "LV " + std::to_string(loadout.level) : "LV 1";
    level.pixel = 0.19f;
    level.x = kXpBarX + 1.0f;
    level.y = kXpBarY;
    level.colour = Rgba{0.92f, 0.94f, 0.98f, 1.0f};
    layer->lines.push_back(level);

    TextLine xp;
    xp.text = loadout_.has_value() ? std::to_string(loadout.xp) + "/" + std::to_string(loadout.xp_next) : "";
    xp.pixel = 0.19f;
    xp.x = kXpBarX + kXpBarWidth - 1.0f -
           static_cast<float>(text_pixel_width(xp.text)) * xp.pixel;
    xp.y = kXpBarY;
    xp.colour = Rgba{0.70f, 0.74f, 0.82f, 1.0f};
    layer->lines.push_back(xp);

    TextLine clock;
    clock.text = "TIME " +
                 format_time(last_run_second_ == 0xFFFFFFFFu ? 0u : last_run_second_ * 1000u);
    clock.pixel = 0.26f;
    clock.x = kViewportHalfWidth - 0.5f -
              static_cast<float>(text_pixel_width(clock.text)) * clock.pixel;
    clock.y = 22.3f;
    clock.colour = Rgba{0.92f, 0.94f, 0.98f, 1.0f};
    layer->lines.push_back(clock);

    rebuild_layer(*layer);
}

void Hud::layout_offer(const LevelUpOffer& offer)
{
    clear_batch(modal_);

    const Rgba kCardInner{0.10f, 0.11f, 0.16f, 0.95f};
    const Rgba kGold{1.00f, 0.80f, 0.30f, 1.0f};
    const Rgba kDim{0.70f, 0.74f, 0.82f, 1.0f};
    const Rgba kWhite{0.92f, 0.94f, 0.98f, 1.0f};

    for (size_t i = 0; i < 3; ++i)
    {
        const UpgradeOptionNet& option = offer.options[i];
        const Rgba accent = option.is_evo ? kGold : item_colour(option.item_id);
        quad_into(modal_, accent, kCardX[i], kCardY, kCardWidth * 0.5f, kCardHeight * 0.5f);
        quad_into(modal_, kCardInner, kCardX[i], kCardY, (kCardWidth - 0.9f) * 0.5f,
                  (kCardHeight - 0.9f) * 0.5f);
    }
    rebuild_batch(modal_);

    TextLayer* title = ensure_layer(modal_title_);
    title->lines.clear();
    TextLine title_line;
    title_line.text = "LEVEL UP!";
    title_line.pixel = 0.55f;
    title_line.x = -static_cast<float>(text_pixel_width(title_line.text)) * title_line.pixel * 0.5f;
    title_line.y = 16.8f;
    title_line.colour = kGold;
    title->lines.push_back(title_line);
    TextLine subtitle;
    subtitle.text = "LEVEL " + std::to_string(offer.new_level) + " - CHOOSE ONE";
    subtitle.pixel = 0.30f;
    subtitle.x = -static_cast<float>(text_pixel_width(subtitle.text)) * subtitle.pixel * 0.5f;
    subtitle.y = 12.4f;
    subtitle.colour = kDim;
    title->lines.push_back(subtitle);
    rebuild_layer(*title);

    // One text layer per card so each card's name carries its own accent.
    for (size_t i = 0; i < 3; ++i)
    {
        const UpgradeOptionNet& option = offer.options[i];
        const Rgba accent = option.is_evo ? kGold : item_colour(option.item_id);
        TextLayer* card = ensure_layer(modal_cards_[i]);
        card->lines.clear();

        float y = 6.3f;
        if (option.is_evo)
        {
            TextLine tag;
            tag.text = "EVOLUTION!";
            tag.pixel = 0.28f;
            tag.x = kCardX[i] - static_cast<float>(text_pixel_width(tag.text)) * tag.pixel * 0.5f;
            tag.y = y;
            tag.colour = kGold;
            card->lines.push_back(tag);
            y -= 8.0f * 0.28f + 0.6f;
        }

        for (const std::string& name_line : split_name(item_name(option.item_id), 10))
        {
            TextLine line;
            line.text = name_line;
            line.pixel = fit_text_pixel(name_line, kCardWidth - 3.0f, 0.0f, 0.34f);
            line.x = kCardX[i] - static_cast<float>(text_pixel_width(name_line)) * line.pixel * 0.5f;
            line.y = y;
            line.colour = accent;
            card->lines.push_back(line);
            y -= 8.0f * line.pixel;
        }

        // What the pick actually changes, as a comparison against what the
        // player has now: the card's reason to exist. Its own layer so the
        // card's text object keeps the section count every offer shares.
        const std::string stat = format_stat_delta(option.stat_type, option.current_stat_fp,
                                                       option.upgraded_stat_fp);
        float stat_pixel = 0.0f;
        if (!stat.empty())
        {
            stat_pixel = fit_text_pixel(stat, kCardWidth - 3.0f, 0.0f, 0.24f);
            TextLayer* stats = ensure_layer(modal_stats_[i]);
            stats->lines.clear();
            TextLine stat_line;
            stat_line.text = stat;
            stat_line.pixel = stat_pixel;
            stat_line.x = kCardX[i] - static_cast<float>(text_pixel_width(stat)) * stat_line.pixel * 0.5f;
            stat_line.y = y;
            stat_line.colour = kWhite;
            stats->lines.push_back(stat_line);
            rebuild_layer(*stats);
            y -= 8.0f * stat_pixel + 0.4f;
        }

        TextLine detail;
        detail.text = option.is_evo
                          ? item_blurb(option.item_id, option.next_level)
                          : (option.next_level <= 1 ? "NEW!"
                                                     : "LEVEL " + std::to_string(option.next_level));
        detail.pixel = fit_text_pixel(detail.text, kCardWidth - 3.0f, 0.0f, 0.26f);
        detail.x = kCardX[i] - static_cast<float>(text_pixel_width(detail.text)) * detail.pixel * 0.5f;
        detail.y = y - 0.9f;
        detail.colour = kDim;
        card->lines.push_back(detail);

        TextLine key;
        key.text = std::to_string(i + 1);
        key.pixel = 0.32f;
        key.x = kCardX[i] - static_cast<float>(text_pixel_width(key.text)) * key.pixel * 0.5f;
        key.y = -6.6f;
        key.colour = kWhite;
        card->lines.push_back(key);

        rebuild_layer(*card);
    }

    TextLayer* footer = ensure_layer(modal_footer_);
    footer->lines.clear();
    TextLine footer_line;
    footer_line.text = "PRESS 1 2 3";
    footer_line.pixel = 0.30f;
    footer_line.x = -static_cast<float>(text_pixel_width(footer_line.text)) * footer_line.pixel * 0.5f;
    footer_line.y = -10.6f;
    footer_line.colour = kDim;
    footer->lines.push_back(footer_line);
    rebuild_layer(*footer);
}

void Hud::layout_game_over(const GameOverStats& stats)
{
    const Rgba kGold{1.00f, 0.80f, 0.30f, 1.0f};
    const Rgba kWhite{0.92f, 0.94f, 0.98f, 1.0f};
    const Rgba kDim{0.70f, 0.74f, 0.82f, 1.0f};

    TextLayer* title = ensure_layer(score_title_);
    title->lines.clear();
    TextLine title_line;
    title_line.text = "GAME OVER";
    title_line.pixel = 0.70f;
    title_line.x = -static_cast<float>(text_pixel_width(title_line.text)) * title_line.pixel * 0.5f;
    title_line.y = 10.5f;
    title_line.colour = kGold;
    title->lines.push_back(title_line);
    rebuild_layer(*title);

    TextLayer* lines = ensure_layer(score_lines_);
    lines->lines.clear();

    TextLine survived;
    survived.text = "SURVIVED " + format_time(stats.seconds_survived * 1000u);
    survived.pixel = 0.42f;
    survived.x = -static_cast<float>(text_pixel_width(survived.text)) * survived.pixel * 0.5f;
    survived.y = 6.6f;
    survived.colour = kWhite;
    lines->lines.push_back(survived);

    TextLine score;
    score.text = "SCORE " + std::to_string(stats.score);
    score.pixel = 0.42f;
    score.x = -static_cast<float>(text_pixel_width(score.text)) * score.pixel * 0.5f;
    score.y = 4.4f;
    score.colour = kWhite;
    lines->lines.push_back(score);

    TextLine kills;
    kills.text = "KILLS " + std::to_string(stats.kills);
    kills.pixel = 0.42f;
    kills.x = -static_cast<float>(text_pixel_width(kills.text)) * kills.pixel * 0.5f;
    kills.y = 2.2f;
    kills.colour = kWhite;
    lines->lines.push_back(kills);
    rebuild_layer(*lines);

    TextLayer* footer = ensure_layer(score_footer_);
    footer->lines.clear();
    TextLine footer_line;
    footer_line.text = "PRESS R TO RESTART";
    footer_line.pixel = 0.36f;
    footer_line.x = -static_cast<float>(text_pixel_width(footer_line.text)) * footer_line.pixel * 0.5f;
    footer_line.y = -8.0f;
    footer_line.colour = kDim;
    footer->lines.push_back(footer_line);
    rebuild_layer(*footer);
}

void Hud::layout_banner(const std::string& name)
{
    const Rgba kGold{1.00f, 0.80f, 0.30f, 1.0f};
    const Rgba kWhite{0.92f, 0.94f, 0.98f, 1.0f};

    TextLayer* tag = ensure_layer(banner_tag_);
    tag->lines.clear();
    TextLine tag_line;
    tag_line.text = "EVOLUTION!";
    tag_line.pixel = 0.60f;
    tag_line.x = -static_cast<float>(text_pixel_width(tag_line.text)) * tag_line.pixel * 0.5f;
    tag_line.y = 8.6f;
    tag_line.colour = kGold;
    tag->lines.push_back(tag_line);
    rebuild_layer(*tag);

    TextLayer* item = ensure_layer(banner_name_);
    item->lines.clear();
    TextLine item_line;
    item_line.text = name;
    item_line.pixel = 0.40f;
    item_line.x = -static_cast<float>(text_pixel_width(item_line.text)) * item_line.pixel * 0.5f;
    item_line.y = 6.4f;
    item_line.colour = kWhite;
    item->lines.push_back(item_line);
    rebuild_layer(*item);
}

void Hud::layout_main_menu()
{
    const Rgba kGold{1.00f, 0.80f, 0.30f, 1.0f};
    const Rgba kWhite{0.92f, 0.94f, 0.98f, 1.0f};
    const Rgba kDim{0.70f, 0.74f, 0.82f, 1.0f};
    const Rgba kRed{0.95f, 0.35f, 0.35f, 1.0f};
    const Rgba kHighlight{0.20f, 0.22f, 0.30f, 0.85f};

    // The two entries, top to bottom, and the highlight that marks the
    // selected one.
    constexpr const char* kOptions[2] = {"SINGLE PLAYER", "MULTIPLAYER"};
    constexpr float kOptionY[2] = {3.5f, -2.0f};
    constexpr float kOptionPixel = 0.48f;

    clear_batch(menu_);
    const float highlight_width =
        static_cast<float>(text_pixel_width(kOptions[menu_selection_])) * kOptionPixel + 2.5f;
    quad_into(menu_, kHighlight, 0.0f, kOptionY[menu_selection_], highlight_width * 0.5f, 1.3f);
    rebuild_batch(menu_);

    TextLayer* title = ensure_layer(menu_title_);
    title->lines.clear();
    TextLine title_line;
    title_line.text = "SHAPED HAVEN";
    title_line.pixel = 0.60f;
    title_line.x = -static_cast<float>(text_pixel_width(title_line.text)) * title_line.pixel * 0.5f;
    title_line.y = 14.0f;
    title_line.colour = kGold;
    title->lines.push_back(title_line);
    TextLine subtitle;
    subtitle.text = "MAIN MENU";
    subtitle.pixel = 0.30f;
    subtitle.x = -static_cast<float>(text_pixel_width(subtitle.text)) * subtitle.pixel * 0.5f;
    subtitle.y = 10.5f;
    subtitle.colour = kDim;
    title->lines.push_back(subtitle);
    rebuild_layer(*title);

    TextLayer* options = ensure_layer(menu_options_);
    options->lines.clear();
    for (int i = 0; i < 2; ++i)
    {
        TextLine line;
        line.text = kOptions[i];
        line.pixel = kOptionPixel;
        line.x = -static_cast<float>(text_pixel_width(line.text)) * line.pixel * 0.5f;
        line.y = kOptionY[i];
        line.colour = (i == menu_selection_) ? kWhite : kDim;
        options->lines.push_back(line);
    }
    rebuild_layer(*options);

    TextLayer* footer = ensure_layer(menu_footer_);
    footer->lines.clear();
    TextLine footer_line;
    footer_line.text = "UP/DOWN SELECT - ENTER CONFIRM";
    footer_line.pixel = 0.28f;
    footer_line.x = -static_cast<float>(text_pixel_width(footer_line.text)) * footer_line.pixel * 0.5f;
    footer_line.y = -9.0f;
    footer_line.colour = kDim;
    footer->lines.push_back(footer_line);
    rebuild_layer(*footer);

    TextLayer* error = ensure_layer(menu_error_);
    error->lines.clear();
    if (!menu_error_text_.empty())
    {
        TextLine error_line;
        error_line.text = menu_error_text_;
        error_line.pixel = fit_text_pixel(error_line.text, 44.0f, 0.0f, 0.30f);
        error_line.x = -static_cast<float>(text_pixel_width(error_line.text)) * error_line.pixel * 0.5f;
        error_line.y = -13.0f;
        error_line.colour = kRed;
        error->lines.push_back(error_line);
    }
    rebuild_layer(*error);
}

void Hud::layout_character_select()
{
    const Rgba kGold{1.00f, 0.80f, 0.30f, 1.0f};
    const Rgba kWhite{0.92f, 0.94f, 0.98f, 1.0f};
    const Rgba kDim{0.55f, 0.60f, 0.70f, 1.0f};
    const Rgba kFieldActive{0.22f, 0.27f, 0.42f, 0.85f};
    const Rgba kFieldIdle{0.10f, 0.11f, 0.16f, 0.80f};
    const Rgba kRowActive{0.30f, 0.36f, 0.55f, 0.90f};
    const Rgba kBoxInner{0.07f, 0.08f, 0.12f, 0.90f};

    const RaceInfo& race = race_info(select_race_);
    const StartingWeaponInfo& weapon = starting_weapon_info(select_weapon_);

    clear_batch(select_fields_);
    clear_batch(select_);

    // The four fields, each a box. The one the highlight is on is lit and the
    // rest stay dark, so the screen reads as four places Up/Down can be. They
    // go in their own batch, below the highlights that mark the chosen race and
    // weapon: two quads at the same z have no order between them, and a
    // highlight that landed behind the box it marks would be invisible.
    const Rgba race_field =
        select_selection_ == kSelectFieldRace ? kFieldActive : kFieldIdle;
    const Rgba weapon_field =
        select_selection_ == kSelectFieldWeapon ? kFieldActive : kFieldIdle;
    const Rgba start_field =
        select_selection_ == kSelectFieldStart ? kFieldActive : kFieldIdle;

    quad_into(select_fields_, select_selection_ == kSelectFieldName ? kFieldActive : kFieldIdle, 0.0f,
              kSelectNameY, 35.0f, 1.5f);
    quad_into(select_fields_, race_field, -20.0f, kSelectRaceBoxY, 15.0f, kSelectRaceBoxHalfH);
    quad_into(select_fields_, weapon_field, 0.0f, kSelectWeaponY, 35.0f, kSelectWeaponHalfH + 0.4f);
    quad_into(select_fields_, start_field, 0.0f, kSelectStartY, 9.0f, 1.5f);
    rebuild_batch(select_fields_);

    // The chosen race's row, so it stays visible while the highlight is on
    // another field, and the four weapon boxes, the chosen one carrying its
    // tray colour.
    const float race_centre = kSelectRaceTop - kSelectRacePixel * 3.5f;
    quad_into(select_, kRowActive, -20.0f, race_centre - static_cast<float>(select_race_) * kSelectRaceStep,
              14.5f, 0.9f);

    for (int i = 0; i < kStartingWeaponCount; ++i)
    {
        const bool chosen = i == select_weapon_;
        quad_into(select_, chosen ? item_colour(static_cast<uint8_t>(i)) : kFieldIdle,
                  kSelectWeaponX[i], kSelectWeaponY, kSelectWeaponHalfW, kSelectWeaponHalfH);
        quad_into(select_, kBoxInner, kSelectWeaponX[i], kSelectWeaponY, kSelectWeaponHalfW - 0.35f,
                  kSelectWeaponHalfH - 0.35f);
    }

    // The caret sits just past the name while the name field is selected. The
    // font has no glyph for one, so it is a quad rather than a character.
    if (select_selection_ == kSelectFieldName)
    {
        const float caret_x = kSelectNameX +
                              static_cast<float>(text_pixel_width(select_name_text_)) * kSelectNamePixel + 0.5f;
        quad_into(select_, kWhite, caret_x, kSelectNameY, 0.12f, 0.85f);
    }
    rebuild_batch(select_);

    TextLayer* title = ensure_layer(select_title_);
    title->lines.clear();
    TextLine title_line;
    title_line.text = "CHOOSE YOUR CHARACTER";
    title_line.pixel = fit_text_pixel(title_line.text, 70.0f, 0.0f, 0.52f);
    title_line.x = -static_cast<float>(text_pixel_width(title_line.text)) * title_line.pixel * 0.5f;
    title_line.y = 21.0f;
    title_line.colour = kGold;
    title->lines.push_back(title_line);
    rebuild_layer(*title);

    TextLayer* name = ensure_layer(select_name_);
    name->lines.clear();
    TextLine name_label;
    name_label.text = "NAME";
    name_label.pixel = 0.34f;
    name_label.x = kSelectRowLeft;
    name_label.y = kSelectNameY + 0.34f * 3.5f;
    name_label.colour = kDim;
    name->lines.push_back(name_label);
    TextLine name_value;
    name_value.text = select_name_text_;
    name_value.pixel = kSelectNamePixel;
    name_value.x = kSelectNameX;
    name_value.y = kSelectNameY + kSelectNamePixel * 3.5f;
    name_value.colour = select_selection_ == kSelectFieldName ? kWhite : kDim;
    name->lines.push_back(name_value);
    rebuild_layer(*name);

    TextLayer* races = ensure_layer(select_races_);
    races->lines.clear();
    for (int i = 0; i < kRaceCount; ++i)
    {
        TextLine line;
        line.text = race_table()[i].name;
        line.pixel = kSelectRacePixel;
        line.x = kSelectRowLeft;
        line.y = kSelectRaceTop - static_cast<float>(i) * kSelectRaceStep;
        line.colour = i == select_race_ ? kWhite : kDim;
        races->lines.push_back(line);
    }
    rebuild_layer(*races);

    TextLayer* detail = ensure_layer(select_detail_);
    detail->lines.clear();
    TextLine race_name;
    race_name.text = race.name;
    race_name.pixel = fit_text_pixel(race_name.text, 34.0f, 0.0f, 0.42f);
    race_name.x = kSelectDetailX;
    race_name.y = kSelectRaceTop;
    race_name.colour = kGold;
    detail->lines.push_back(race_name);

    const std::string stats[5] = {
        "HP " + std::to_string(race.max_hp),
        "SHIELD " + std::to_string(race.max_shield),
        "MANA " + std::to_string(race.max_mana),
        "SPEED " + format_multiplier(race.move_speed_mult),
        "ATK " + format_multiplier(race.damage_mult),
    };
    for (int i = 0; i < 5; ++i)
    {
        TextLine line;
        line.text = stats[i];
        line.pixel = 0.30f;
        line.x = kSelectDetailX;
        line.y = 9.8f - static_cast<float>(i) * 1.7f;
        line.colour = kWhite;
        detail->lines.push_back(line);
    }

    TextLine weapon_header;
    weapon_header.text = "WEAPON";
    weapon_header.pixel = 0.30f;
    weapon_header.x = kSelectDetailX;
    weapon_header.y = -0.2f;
    weapon_header.colour = kDim;
    detail->lines.push_back(weapon_header);

    TextLine weapon_blurb;
    weapon_blurb.text = std::string(weapon.name) + " - " + weapon.description;
    weapon_blurb.pixel = fit_text_pixel(weapon_blurb.text, 40.0f, 0.0f, 0.30f);
    weapon_blurb.x = -static_cast<float>(text_pixel_width(weapon_blurb.text)) * weapon_blurb.pixel * 0.5f;
    weapon_blurb.y = -12.2f;
    weapon_blurb.colour = kWhite;
    detail->lines.push_back(weapon_blurb);
    rebuild_layer(*detail);

    TextLayer* weapons = ensure_layer(select_weapons_);
    weapons->lines.clear();
    for (int i = 0; i < kStartingWeaponCount; ++i)
    {
        TextLine line;
        line.text = starting_weapon_table()[i].name;
        line.pixel = 0.36f;
        line.x = kSelectWeaponX[i] - static_cast<float>(text_pixel_width(line.text)) * line.pixel * 0.5f;
        line.y = kSelectWeaponY + line.pixel * 3.5f;
        line.colour = i == select_weapon_ ? kWhite : kDim;
        weapons->lines.push_back(line);
    }
    rebuild_layer(*weapons);

    TextLayer* start = ensure_layer(select_start_);
    start->lines.clear();
    TextLine start_line;
    start_line.text = "START";
    start_line.pixel = 0.42f;
    start_line.x = -static_cast<float>(text_pixel_width(start_line.text)) * start_line.pixel * 0.5f;
    start_line.y = kSelectStartY + start_line.pixel * 3.5f;
    start_line.colour = select_selection_ == kSelectFieldStart ? kGold : kWhite;
    start->lines.push_back(start_line);
    rebuild_layer(*start);

    TextLayer* hints = ensure_layer(select_hints_);
    hints->lines.clear();
    const char* const kHints[2] = {
        "UP/DOWN FIELD - LEFT/RIGHT CHANGE VALUE",
        "TYPE NAME - ENTER CONFIRM - ESC BACK",
    };
    for (int i = 0; i < 2; ++i)
    {
        TextLine line;
        line.text = kHints[i];
        line.pixel = fit_text_pixel(line.text, 74.0f, 0.0f, 0.26f);
        line.x = -static_cast<float>(text_pixel_width(line.text)) * line.pixel * 0.5f;
        line.y = -18.4f - static_cast<float>(i) * 2.2f;
        line.colour = kDim;
        hints->lines.push_back(line);
    }
    rebuild_layer(*hints);
}

}  // namespace haven

