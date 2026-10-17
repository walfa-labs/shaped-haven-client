#pragma once

#include "entity_frame.h"
#include "game_math.h"
#include "hud_model.h"
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
class CelShaderGenerator;
class AnthropomorphicMeshBuilder;

/// The 2D overlay: the status cluster (health, shield, mana), the XP bar, the
/// item tray, the level-up modal, the evolution banner, the main menu, the
/// character select and the game-over scoreboard. Everything is drawn from the
/// same immediate-mode PrimitiveSink the combat batches use — text included,
/// through the baked 5x7 font in hud_text — so the HUD is one more set of
/// Manual Object sections rather than a second rendering path.
///
/// Layout is in world units of the fixed 80 x 45 orthographic viewport
/// (x in [-40, 40], y in [-22.5, 22.5]); the camera keeps the action centred,
/// so screen space and world space coincide.
class Hud {
public:
    Hud(Ogre::SceneManager* scene_manager, MeshFactory* meshes, Ogre::SceneNode* camera_node,
        CelShaderGenerator* cel, AnthropomorphicMeshBuilder* race_meshes);
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

    /// The local player's vitals, from the newest snapshot's own player record.
    /// Drives the health, shield and mana bars. Call once per frame.
    void set_vitals(const Vitals& vitals);

    /// The player tried to dash without the mana to pay for it: the mana bar
    /// flashes white for BarAnimator::kManaFlashSeconds.
    void signal_dash_denied();

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
    /// full-screen element: the item tray boxes, the modal cards and the HUD
    /// bars. Rebuilt wholesale whenever it changes, sectioned per colour like
    /// the batches. `z` places the batch among the other HUD layers: quads of
    /// two batches at the same z have no defined order between them, so a
    /// batch that must sit over another is given a higher one.
    struct QuadBatch {
        Ogre::ManualObject* object = nullptr;
        Ogre::SceneNode* node = nullptr;
        std::string name;
        float z = 0.49f;
        /// The batch node's origin and rotation, in the HUD's camera-local
        /// space. Zero for the batches that live directly in that space; the
        /// status cluster uses them to rotate its three bars about the
        /// cluster's own centre, which is what keeps a rotated bar from
        /// swinging off the edge of the viewport.
        float x = 0.0f;
        float y = 0.0f;
        float angle = 0.0f;
        struct Section {
            Rgba colour;
            /// The section's depth offset above the batch's own `z`. Zero is the
            /// batch's z; a bar's layers pass an explicit one, because two
            /// coplanar quads have no defined order between them and a layer
            /// that has to sit over another cannot rely on which section
            /// happens to be drawn last.
            float z = 0.0f;
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
    void quad_into(QuadBatch& batch, Rgba colour, float cx, float cy, float hx, float hy, float z = 0.0f);
    /// The same quad from four explicit corners, which is how the skewed bars
    /// are emitted: their corners are not an axis-aligned rectangle. `z` is the
    /// section's depth offset above the batch's own.
    void quad_corners_into(QuadBatch& batch, Rgba colour, const float xs[4], const float ys[4],
                           float z = 0.0f);
    void rebuild_batch(QuadBatch& batch);
    void clear_batch(QuadBatch& batch);

    // Persona-styled bars. Every bar is a parallelogram — a rectangle sheared
    // along x — carrying a black outer border, a white inner border and a dark
    // track for its fill, and the whole status cluster is rotated about its
    // own centre.
    ///
    /// One layered box: black outer border, white inner border, the box's own
    /// colour, then its dark interior. `border` is the thickness of each rim.
    void layered_box_into(QuadBatch& batch, Rgba accent, Rgba inner, float x, float y, float w,
                          float h, float border);
    /// One bar's frame: borders and track, with `hx`/`hy` the track's half
    /// extents.
    void bar_frame_into(QuadBatch& batch, float cx, float cy, float hx, float hy, float shear);
    /// One bar's fill, anchored at the bar's left edge and covering `fraction`
    /// of its width. `inset` pulls the fill in from the track so the white
    /// border stays visible around it, and `z` is the section's depth offset.
    void bar_fill_into(QuadBatch& batch, float cx, float cy, float hx, float hy, float shear,
                       float fraction, Rgba colour, float inset, float z = 0.0f);
    /// One hexagonal chevron divider, the shield bar's segment mark.
    void hexagon_into(QuadBatch& batch, Rgba colour, float cx, float cy, float radius, float thickness,
                      float shear);
    /// The mana bar's cobalt-to-violet gradient at `t` along the bar.
    static Rgba mana_gradient(float t);

    // Persistent elements.
    void build_persistent_elements();
    void layout_tray(const Loadout& loadout);
    void rebuild_hud_lines();

    // The status cluster: XP bar, health, shield and mana.
    void refresh_vitals();
    bool vitals_dirty() const;
    void layout_vitals();
    void layout_xp_bar();
    void layout_status_bars();

    // Modal, scoreboard and banner layout.
    void layout_offer(const LevelUpOffer& offer);
    void layout_game_over(const GameOverStats& stats);
    void layout_banner(const std::string& name);
    void layout_main_menu();
    void layout_character_select();

    Ogre::SceneManager* scene_ = nullptr;
    MeshFactory* meshes_ = nullptr;
    Ogre::SceneNode* camera_node_ = nullptr;
    CelShaderGenerator* cel_ = nullptr;
    AnthropomorphicMeshBuilder* race_meshes_ = nullptr;

    /// The character select's cel-shaded preview: the chosen race's
    /// anthropomorphic mesh on a camera-local node in front of the panel,
    /// turning slowly so the silhouette reads from every angle. Null until
    /// first shown, and when the cel-shading shaders failed to compile.
    Ogre::Entity* preview_ = nullptr;
    Ogre::SceneNode* preview_node_ = nullptr;
    Ogre::MaterialPtr preview_material_;
    int preview_race_ = 0;
    float preview_angle_ = 0.0f;

    /// Builds the preview entity on first use and points it at the selected
    /// race's mesh; a no-op without cel-shading or before the first race.
    void ensure_preview();
    void destroy_preview();

    /// Child of the camera node that cancels its z (kCameraZ), so every HUD
    /// element parented under it lands at the z the combat layers use while
    /// still following the camera's x/y.
    Ogre::SceneNode* attach_node_ = nullptr;

    Ogre::Entity* dim_ = nullptr;
    Ogre::Entity* panel_ = nullptr;
    /// The panel's jagged double border: black outside, white inside, drawn
    /// around `panel_` so the modals carry the same frame the bars do.
    Ogre::Entity* panel_border_outer_ = nullptr;
    Ogre::Entity* panel_border_inner_ = nullptr;

    QuadBatch tray_;
    QuadBatch modal_;
    QuadBatch menu_;
    /// The XP bar: the one bar that is not rotated, because a bar spanning the
    /// whole 80-unit viewport cannot be turned -8 degrees about its centre
    /// without pushing its ends past the top and bottom of the 45-unit window.
    /// It carries the same frame, fill and crimson accent as the others.
    QuadBatch xp_bar_;
    /// The three status bars, rotated about the cluster's centre: the shield
    /// above health, the mana bar beneath it.
    QuadBatch status_;
    /// The shield bar's chevron dividers. A batch of their own because they sit
    /// on the shield fill, and a divider drawn in the fill's own batch could
    /// render behind it.
    QuadBatch status_decals_;
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

    /// The animated bar state behind the status cluster.
    BarAnimator bars_;
    /// The XP bar's own state, mirroring what the persistent quads used to
    /// hold: the frame stays up through a run even when there is no fill to
    /// draw on it.
    bool xp_bar_visible_ = true;
    bool xp_fill_visible_ = false;
    float xp_fraction_ = 0.0f;

    /// The frame the bars were last laid out from, so `refresh_vitals()` can
    /// rebuild only when something actually moved. The sentinel values make
    /// the first refresh always rebuild.
    struct LaidOut {
        bool status_visible = false;
        bool shield_visible = false;
        bool flashing = false;
        bool xp_visible = false;
        bool xp_fill_visible = false;
        float hp = -1.0f;
        float ghost = -1.0f;
        float shield = -1.0f;
        float mana = -1.0f;
        float xp = -1.0f;
    };
    LaidOut laid_;

    /// Positions and scales the panel and its border are laid out with, so the
    /// three screens that show a panel cannot drift apart.
    void set_panel(float half_width, float half_height, float centre_y);
    void hide_panel();

    struct ColourLess {
        bool operator()(const std::array<float, 4>& a, const std::array<float, 4>& b) const { return a < b; }
    };
    std::map<std::array<float, 4>, Ogre::MaterialPtr, ColourLess> material_cache_;

    static constexpr float kBannerSeconds = 2.5f;
};

}  // namespace haven
