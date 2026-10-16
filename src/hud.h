#pragma once

#include "entity_frame.h"
#include "game_math.h"
#include "item_catalog.h"
#include "protocol.h"

#include <OgreManualObject.h>
#include <OgreMaterial.h>
#include <OgrePrerequisites.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>

#include <array>
#include <cstdint>
#include <string>

namespace haven {

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
    Hud(Ogre::SceneManager* scene_manager, class MeshFactory* meshes);
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

    /// Advances the banner countdown; call once per rendered frame.
    void tick(float dt_seconds);

private:
    struct TextLine {
        std::string text;
        float x = 0.0f;  // left edge
        float y = 0.0f;  // top edge
        float pixel = 0.2f;
        Rgba colour{0.92f, 0.94f, 0.98f, 1.0f};
    };

    /// One Manual Object of text quads, one section per line, sized to the
    /// exact lit-pixel count and rebuilt only when a line changes.
    struct TextLayer {
        Ogre::ManualObject* object = nullptr;
        std::vector<TextLine> lines;
    };

    /// One Manual Object for every coloured quad that is not a persistent
    /// full-screen element: the item tray boxes and the modal cards. Rebuilt
    /// wholesale whenever it changes, sectioned per colour like the batches.
    struct QuadBatch {
        Ogre::ManualObject* object = nullptr;
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

    Ogre::SceneManager* scene_ = nullptr;
    MeshFactory* meshes_ = nullptr;

    Ogre::Entity* xp_bg_ = nullptr;
    Ogre::Entity* xp_fill_ = nullptr;
    Ogre::Entity* dim_ = nullptr;
    Ogre::Entity* panel_ = nullptr;

    QuadBatch tray_;
    QuadBatch modal_;
    TextLayer* hud_lines_ = nullptr;    // level label, xp numbers, run clock
    TextLayer* tray_text_ = nullptr;    // weapon/passive names
    TextLayer* modal_title_ = nullptr;  // "LEVEL UP!" header
    std::array<TextLayer*, 3> modal_cards_{};
    TextLayer* modal_footer_ = nullptr;
    TextLayer* score_title_ = nullptr;
    TextLayer* score_lines_ = nullptr;
    TextLayer* score_footer_ = nullptr;
    TextLayer* banner_tag_ = nullptr;
    TextLayer* banner_name_ = nullptr;

    const Loadout* loadout_ = nullptr;
    std::array<uint8_t, 4> last_evolved_{};
    uint32_t last_run_second_ = 0xFFFFFFFFu;
    bool banner_visible_ = false;
    float banner_timer_ = 0.0f;
    bool offer_visible_ = false;
    bool game_over_visible_ = false;
    bool initialised_ = false;

    static constexpr float kBannerSeconds = 2.5f;
};

}  // namespace haven
