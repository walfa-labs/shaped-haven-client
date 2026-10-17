#pragma once

#include "entity_frame.h"
#include "game_math.h"
#include "item_catalog.h"
#include "levelup_format.h"
#include "protocol.h"

#include <OgreManualObject.h>
#include <OgreMaterial.h>
#include <OgrePrerequisites.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace haven {

class MeshFactory;

/// The 2D overlay: XP bar, item tray, level-up modal, evolution banner and the
/// game-over scoreboard. Everything is drawn from the same immediate-mode
/// PrimitiveSink the combat batches use — text included, through the baked
/// 5x7 font in hud_text — so the HUD is one more set of Manual Object
/// sections rather than a second rendering path.
///
/// Layout is in world units of the fixed 80 x 45 orthographic viewport
/// (x in [-40, 40], y in [-22.5, 22.5]); the camera keeps the action centred,
/// so screen space and world space coincide.
class Hud {
public:
    Hud(Ogre::SceneManager* scene_manager, MeshFactory* meshes, Ogre::SceneNode* camera_node);
    ~Hud();

    Hud(const Hud&) = delete;
    Hud& operator=(const Hud&) = delete;

    /// Called every frame with the newest snapshot's run clock. Drives the XP
    /// bar fill and the run-time readout.
    void set_run_time(uint32_t run_time_ms);

    /// Loads the item tray and XP bar from S2C_LOADOUT. Also makes an
    /// evolution visible when it happens: a weapon slot whose `evolved` byte
    /// just became 1 raises the banner.
    void set_loadout(const Loadout& loadout);

    /// A copy of the newest S2C_LOADOUT. The wire struct arrives by const
    /// reference to a caller-owned local, so the HUD keeps its own copy
    /// rather than a pointer that dangles the moment the handler returns.
    std::optional<Loadout> loadout_;

    // Level-up modal.
    void show_offer(const LevelUpOffer& offer);
    void hide_offer();
    bool offer_visible() const { return offer_visible_; }

    // Evolution banner, self-hiding after kBannerSeconds.
    void show_evolution(uint8_t evo_item_id);

    // Game-over scoreboard.
    void show_game_over(const GameOverStats& stats);
    void hide_game_over();
    bool game_over_visible() const { return game_over_visible_; }

    /// Clears every run-bound element back to the pre-run state: called when
    /// C2S_RESTART goes out, so the next snapshot starts from a clean HUD.
    void reset();

    // Main menu, shown before a run exists and therefore before any server
    // connection has been made.
    void show_main_menu();
    void hide_main_menu();
    bool menu_visible() const { return menu_visible_; }

    /// Which entry is highlighted: 0 for the first, 1 for the second.
    void set_menu_selection(int selection);

    /// The connection-failure banner. Empty means no banner.
    void set_menu_error(const std::string& error);
    void clear_menu_error();

    // Character select, shown between the main menu and the connection. It
    // mirrors the name, race, starting weapon and field the player has chosen
    // so far; the values themselves live in CharacterSelectModel.
    void show_character_select();
    void hide_character_select();

    /// Which field is highlighted: 0 name, 1 race, 2 weapon, 3 START.
    void set_character_select_selection(int selection);
    void set_character_select_name(const std::string& name);
    void set_character_select_race(int race_id);
    void set_character_select_weapon(int weapon_id);

    /// Destroys every HUD scene object while Ogre is still alive. Must be
    /// called before renderer.closeApp(); the destructor has nothing left to
    /// do afterwards.
    void shutdown();

    /// Advances the banner countdown; call once per rendered frame.
    void tick(float dt_seconds);

    /// The level-up card's stat comparison: delegates to the Ogre-free
    /// haven::format_stat_delta (levelup_format.h), which is unit-tested
    /// without a renderer.
    static std::string format_stat_delta(uint8_t stat_type, int16_t current_stat_fp, int16_t upgraded_stat_fp)
    {
        return haven::format_stat_delta(stat_type, current_stat_fp, upgraded_stat_fp);
    }

private:
    struct TextLine {
        std::string text;
        float x = 0.0f;  // left edge
        float y = 0.0f;  // top edge
        float pixel = 0.2f;
        Rgba colour{0.92f, 0.94f, 0.98f, 1.0f};
    };

    /// One Manual Object of text quads, one section per line, sized to the
    /// exact lit-pixel count and rebuilt only when a line changes. `name` keeps
    /// the per-layer objects distinct: Ogre resources are keyed by name.
    struct TextLayer {
        Ogre::ManualObject* object = nullptr;
        Ogre::SceneNode* node = nullptr;
        std::string name;
        std::vector<TextLine> lines;
    };

    /// One Manual Object for every coloured quad that is not a persistent
    /// full-screen element: the item tray boxes and the modal cards. Rebuilt
    /// wholesale whenever it changes, sectioned per colour like the batches.
    /// `z` places the batch among the other HUD layers: quads of two batches at
    /// the same z have no defined order between them, so a batch that must sit
    /// over another is given a higher one.
    struct QuadBatch {
        Ogre::ManualObject* object = nullptr;
        Ogre::SceneNode* node = nullptr;
        std::string name;
        float z = 0.49f;
        struct Section {
            Rgba colour;
            uint32_t verts = 0;
            uint32_t indices = 0;
            std::vector<float> xs;  // 4 corners per quad
            std::vector<float> ys;
        };
        std::vector<Section> sections;
    };

    Ogre::MaterialPtr material_for(Rgba colour);
    // Text layers. Each logical element owns its layer pointer; a null
    // pointer means the element is not on screen.
    TextLayer* ensure_layer(TextLayer*& ptr);
    void rebuild_layer(TextLayer& layer);
    void clear_layer(TextLayer& layer);
    void destroy_layer(TextLayer*& ptr);

    // Quad batch.
    void quad_into(QuadBatch& batch, Rgba colour, float cx, float cy, float hx, float hy);
    void rebuild_batch(QuadBatch& batch);
    void clear_batch(QuadBatch& batch);

    // Persistent elements.
    void build_persistent_elements();
    void layout_tray(const Loadout& loadout);
    void rebuild_hud_lines();

    // Modal, scoreboard and banner layout.
    void layout_offer(const LevelUpOffer& offer);
    void layout_game_over(const GameOverStats& stats);
    void layout_banner(const std::string& name);
    void layout_main_menu();
    void layout_character_select();

    Ogre::SceneManager* scene_ = nullptr;
    MeshFactory* meshes_ = nullptr;
    Ogre::SceneNode* camera_node_ = nullptr;

    /// Child of the camera node that cancels its z (kCameraZ), so every HUD
    /// element parented under it lands at the z the combat layers use while
    /// still following the camera's x/y.
    Ogre::SceneNode* attach_node_ = nullptr;

    Ogre::Entity* xp_bg_ = nullptr;
    Ogre::Entity* xp_fill_ = nullptr;
    Ogre::Entity* dim_ = nullptr;
    Ogre::Entity* panel_ = nullptr;

    QuadBatch tray_;
    QuadBatch modal_;
    QuadBatch menu_;
    /// The character select's field boxes, and the highlights drawn over them.
    /// They are two batches because a highlight that shared a z with the box it
    /// marks could render behind it.
    QuadBatch select_fields_;
    QuadBatch select_;
    TextLayer* hud_lines_ = nullptr;    // level label, xp numbers, run clock
    TextLayer* tray_text_ = nullptr;    // weapon/passive names
    TextLayer* modal_title_ = nullptr;  // "LEVEL UP!" header
    std::array<TextLayer*, 3> modal_cards_{};
    /// The stat comparison line each card carries ("12 ATK -> 15 ATK"). Own
    /// layer per card rather than a line inside the card's layer: the card
    /// layer keeps the section count it was laid out with, and the stat is
    /// the one line whose presence varies per offer.
    std::array<TextLayer*, 3> modal_stats_{};
    TextLayer* modal_footer_ = nullptr;
    TextLayer* score_title_ = nullptr;
    TextLayer* score_lines_ = nullptr;
    TextLayer* score_footer_ = nullptr;
    TextLayer* banner_tag_ = nullptr;
    TextLayer* banner_name_ = nullptr;
    TextLayer* menu_title_ = nullptr;   // "SHAPED HAVEN" / "MAIN MENU"
    TextLayer* menu_options_ = nullptr; // the two entries
    TextLayer* menu_footer_ = nullptr;  // the key hints
    TextLayer* menu_error_ = nullptr;   // the connection-failure banner
    TextLayer* select_title_ = nullptr;   // "CHOOSE YOUR CHARACTER"
    TextLayer* select_name_ = nullptr;    // the name field's label and value
    TextLayer* select_races_ = nullptr;   // the ten race names
    TextLayer* select_detail_ = nullptr;  // the chosen race's stats
    TextLayer* select_weapons_ = nullptr; // the four weapon names
    TextLayer* select_start_ = nullptr;   // the START button
    TextLayer* select_hints_ = nullptr;   // the key hints

    uint32_t last_run_second_ = 0xFFFFFFFFu;
    bool banner_visible_ = false;
    float banner_timer_ = 0.0f;
    bool offer_visible_ = false;
    bool game_over_visible_ = false;
    bool menu_visible_ = false;
    int menu_selection_ = 0;
    std::string menu_error_text_;
    bool select_visible_ = false;
    int select_selection_ = 0;
    std::string select_name_text_;
    int select_race_ = 0;
    int select_weapon_ = 0;
    bool initialised_ = false;

    struct ColourLess {
        bool operator()(const std::array<float, 4>& a, const std::array<float, 4>& b) const { return a < b; }
    };
    std::map<std::array<float, 4>, Ogre::MaterialPtr, ColourLess> material_cache_;

    static constexpr float kBannerSeconds = 2.5f;
};

}  // namespace haven
