// Copyright 2026 Poly
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Places the game's bottom-screen UI elements onto the main screen ("remaster" layout).
//
// A scene is a bottom-screen context (choice bubble, keyboard, town menu, ...). It becomes active
// when the UI probe sees one of its textures being drawn, and then contributes elements: crops of
// the bottom screen that are drawn at a position relative to the main screen and, if touchable,
// receive touches that are mapped back to the matching bottom-screen coordinates.

#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include "common/common_types.h"
#include "common/math_util.h"
#include "core/frontend/framebuffer_layout.h"

namespace Loader {
class AppLoader;
}

namespace Leaf::Ui {

struct Element {
    std::string id;
    Common::Rectangle<float> src; ///< bottom-screen pixels (320x240, y down)
    float anchor_x, anchor_y;     ///< point on the main screen (0..1 of its rect)
    float pivot_x, pivot_y;       ///< point of the element placed on the anchor (0..1)
    float offset_x = 0.0f;        ///< extra offset in top-screen pixels (400x240 units)
    float offset_y = 0.0f;
    float scale = 1.0f;           ///< 1 = same size relative to the main screen as on a 3DS
    bool fit = false;             ///< scale to fit the main screen instead (takeover elements)
    bool touchable = true;
    float key_tolerance = 0.06f;  ///< alpha threshold after un-mixing; 0 draws the crop opaque
    float key_softness = 0.08f;
    float scrim = 0.0f;           ///< darken the game behind keyed areas (0..1)
    bool backdrop = false;        ///< draw only a dimming layer over the whole main screen
    float opacity = 1.0f;
    /// Texture patterns whose measured on-screen rectangle (union, padded by `bounds_pad` and
    /// clamped to `bounds_clamp`) replaces `src` each frame, for parts whose size or position
    /// the game changes (variable-width bubbles, the expanding map). `src` is the fallback.
    std::vector<std::string> bounds_of = {};
    float bounds_pad = 1.0f;
    /// Measure only images found under `bounds_of` alone (when there are any): generic parts
    /// shared with other screens would stretch the bounds over unrelated UI.
    bool bounds_exclusive = false;
    Common::Rectangle<float> bounds_clamp{0.0f, 0.0f, 320.0f, 240.0f};
    /// Place with the same bottom-screen -> main-screen mapping as this (earlier) element, so
    /// a part sticking out of it stays attached. Not shown while that element is not.
    std::string same_transform_as = {};
    /// With `same_transform_as`: `src` is relative to that element's top-left corner.
    bool src_relative = false;
    /// Touches anywhere on this element reach the game at this bottom-screen point (pixels),
    /// e.g. to keep them off a hidden button next to the intended one. Negative: as placed.
    float touch_x = -1.0f;
    float touch_y = -1.0f;
    /// Put this element's right edge `offset_x` units left of the first of these (earlier)
    /// elements that is shown.
    std::vector<std::string> left_of = {};
    /// Put this element's bottom edge `offset_y` units above that (earlier) element's top.
    std::string above = {};
    /// Only shown when `bounds_of` measured something (parts that exist in some game versions
    /// only, e.g. Welcome amiibo's buttons).
    bool requires_bounds = false;
    /// Only shown while one of these textures is on the bottom screen (e.g. the map, which is
    /// not there indoors). Never shown if the game version has none of them.
    std::vector<std::string> requires_drawn = {};
    /// The anchor is a point of the 2D art on the 3DS top screen (5:3), and moves with the art
    /// when it is rescaled for 16:9 / 4:3 (x around the centre, y around the bottom edge).
    bool follow_art = false;
    /// `src` is in TOP-screen pixels (400x240): a part of the top screen shown over the
    /// bottom-screen parts (never touchable).
    bool from_top = false;
    /// With `from_top`: drawn exactly where that part is on the main screen (anchors ignored),
    /// to bring it in front of bottom-screen parts covering it.
    bool top_in_place = false;
    /// Only shown while the scene's overlay is open (see SetOverlayOpen).
    bool overlay = false;
    /// 1 = touching this element also opens the overlay, 2 = it closes the overlay, 3 = it opens
    /// it (2 and 3 are Leaf buttons, drawn by the frontend with `icon`, not game touches).
    u8 action = 0;
    u8 icon = 0; ///< Icon of a Leaf button
    /// With IconRoundCorners: the radius (in `src` pixels) and how much the corners are dimmed.
    float corner_radius = 0.0f;
    float corner_shade = 0.0f;
};

/// Icons of Leaf's own buttons (drawn by the frontend).
enum Icon : u8 {
    IconNone = 0,
    IconClose = 1,    ///< x
    IconExpand = 2,   ///< show bigger
    IconCollapse = 3, ///< back to the small view
    /// Not a button: the frontend dims this region's corners outside a rounded rectangle, for
    /// a rectangular crop of a rounded window over a dimmed backdrop.
    IconRoundCorners = 4,
};

struct Scene {
    std::string id;
    /// Texture patterns ("Dir/file.arc:" prefixes or full "Dir/file.arc:tex.bclim" names).
    /// The scene is active while any texture unique to these patterns is on the bottom screen.
    std::vector<std::string> textures;
    std::vector<Element> elements;
    bool hide_while_moving = false;
    int priority = 0; ///< higher priority scenes suppress lower ones when active
    /// Texture patterns (same syntax as `textures`) that must ALL be matched by something on
    /// screen. Unlike `textures` these may be shared with other layouts; used to tell apart
    /// contexts built from the same parts.
    std::vector<std::string> requires_all = {};
    /// Stay hidden for a moment after another scene was shown (its closing animation can look
    /// like this scene's textures for a few frames).
    std::string suppressed_after = {};
    float suppress_seconds = 0.0f;
    /// Texture patterns that, when drawn on the TOP screen, hide this scene (e.g. a dialog box).
    std::vector<std::string> hidden_when_top = {};
    /// Only while the game draws text on the bottom screen (any texture that is not layout
    /// art is a font sheet); for layouts shown before their text is filled in.
    bool requires_text = false;
    /// Texture patterns that, when drawn on the bottom screen, mean this scene is not up
    /// (another context built from the same parts).
    std::vector<std::string> excluded_by = {};
    /// Hide after the player presses A once (e.g. a picture shown at the start of a dialog
    /// that the game keeps on the bottom screen until the conversation ends).
    bool dismiss_on_confirm = false;
    /// Texture patterns that must be drawn on the TOP screen for the scene to show (e.g. the
    /// town HUD appears together with the game's own clock plate).
    std::vector<std::string> shown_with_top = {};
    /// Bottom-screen texture patterns while which `shown_with_top` is not needed (the game hides
    /// its element at times there, e.g. the clock while the house light switches); the scene
    /// then hides while the player walks instead.
    std::vector<std::string> shown_without_top_with = {};
    /// Disappear at once instead of fading out (full-screen views: once the menu has closed the
    /// bottom screen already shows something else, which must not flash up at full size).
    bool instant_out = false;
    /// Only while the scene's own textures are drawn inside the bottom screen (the game keeps
    /// drawing the keyboard off-screen while it shows a preview of the text).
    bool requires_onscreen = false;
    /// Texture patterns of which one must be drawn on the TOP screen for the scene to be up
    /// (pages that share their bottom screen with others, told apart by their top screen).
    std::vector<std::string> requires_top = {};
    /// With an overlay (elements marked `overlay`): A opens it too (e.g. a selected list entry's
    /// details; not where A is the game's action button).
    bool overlay_on_confirm = false;
    /// While the overlay is open, the other elements are not shown (instead of staying behind
    /// it), and the scene stays up while the player walks.
    bool overlay_replaces = false;
};

/// Resolves scene texture patterns against the running game's textures. Called at boot.
void Initialize(Loader::AppLoader& loader);
void Shutdown();

/// Reported by the frontend's input handling (circle pad / d-pad held).
void SetMovementInput(bool moving);

/// Reported by the frontend when A is pressed (also opens a scene's overlay).
void NotifyConfirm();

/// Overlay of the current scene (e.g. the details of a selected cat task over the list).
void SetOverlayOpen(bool open);
bool OverlayOpen();
struct Button {
    u8 icon;
    Common::Rectangle<u32> rect; ///< window pixels
    float corner_radius;         ///< IconRoundCorners: window pixels
    float corner_shade;
};
/// Leaf's own buttons currently on screen (open / close an overlay) for a window of this size.
std::vector<Button> Buttons(u32 width, u32 height);

/// Advances fades and returns this frame's regions for the given layout. Renderer thread.
/// `forced_dt` (seconds) replaces the wall-clock frame time when >= 0 (tooling).
std::vector<Layout::ScreenRegion> FrameRegions(const Layout::FramebufferLayout& layout,
                                               float forced_dt = -1.0f);

/// Regions produced by the last FrameRegions call for a window of this size (touch hit-testing).
std::vector<Layout::ScreenRegion> LastRegions(u32 width, u32 height);

/// Maps a bottom-screen point (pixels, 320x240) to window pixels through the regions last
/// drawn for a window of this size. Returns false if no region shows that point.
bool BottomToWindow(u32 width, u32 height, float bottom_x, float bottom_y, float& window_x,
                    float& window_y);

/// True while a scene with this id is (at least half) visible.
bool SceneVisible(const std::string& id);

/// Archive and name of a layout texture key ("Dir/file.arc:name.bclim"), empty if unknown.
std::string TextureName(u64 key);

/// Ids of the currently active scenes (for the frontend / debugging).
std::vector<std::string> ActiveScenes();

} // namespace Leaf::Ui
