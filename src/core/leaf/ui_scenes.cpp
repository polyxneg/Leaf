// Copyright 2026 Poly
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <algorithm>
#include <atomic>
#include <limits>
#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <fmt/format.h>
#include "common/logging/log.h"
#include "core/leaf/leaf.h"
#include "core/leaf/ui_patcher.h"
#include "core/leaf/ui_probe.h"
#include "core/leaf/ui_scenes.h"

namespace Leaf::Ui {

namespace {

using Clock = std::chrono::steady_clock;

constexpr float FadeInSeconds = 0.18f;
constexpr float FadeOutSeconds = 0.25f;
constexpr float StillSecondsBeforeHud = 0.6f;
constexpr float FallbackSettleSeconds = 0.35f;
constexpr float NativeTopHeight = 240.0f;

// Scene table. Element coordinates are bottom-screen pixels; placement is relative to the main
// screen so the same definition works for 16:9, 4:3 and 5:3.
std::vector<Scene> BuildScenes() {
    std::vector<Scene> scenes;

    // Yes/No style choices. The bubble docks at the right corner of the top-screen dialog box,
    // slightly smaller, without the wallpaper behind it (keyed out by the layout patch). The
    // game sizes the bubble to its longest option, so it is cropped to its measured size and
    // placed by its bottom-right corner: it grows up and to the left, never off-screen.
    {
        Scene s{"choice", {"selWin/slct_win.arc:"}, {}, false, 5};
        Element bubble;
        bubble.id = "choice_bubble";
        bubble.src = {40.0f, 1.0f, 280.0f, 216.0f}; // row 0 is not drawn by the game
        bubble.bounds_of = {"selWin/slct_win.arc:"};
        bubble.bounds_exclusive = true; // not the plain parts other screens share
        bubble.bounds_pad = 3.0f;
        bubble.bounds_clamp = {0.0f, 1.0f, 320.0f, 240.0f};
        bubble.follow_art = true; // the dialog box is top-screen art
        bubble.anchor_x = 0.965f; // right end of the dialog box
        bubble.anchor_y = 0.54f;  // just over its top edge
        bubble.pivot_x = 1.0f;
        bubble.pivot_y = 1.0f;
        bubble.scale = 0.8f;
        bubble.opacity = 0.96f;
        s.elements.push_back(bubble);
        scenes.push_back(std::move(s));
    }

    // A choice asked over a map (Nook: "Does this location suit your needs?"): the map and the
    // bubble drawn over it are shown together, as one picture above the dialog box, so the
    // place being decided on stays visible.
    {
        Scene s{"choice_map", {"selWin/slct_win.arc:"}, {}, false, 6, {"Menu/map_*"}};
        s.excluded_by = {"Menu/menu_tab.arc:my_men_btn_00.bclim"}; // not the town HUD's map
        Element both;
        both.id = "choice_map";
        both.src = {0.0f, 0.0f, 320.0f, 224.0f};
        both.bounds_of = {"*"};
        both.bounds_pad = 2.0f;
        both.follow_art = true;
        both.anchor_x = 0.5f;
        both.anchor_y = 0.485f;
        both.pivot_x = 0.5f;
        both.pivot_y = 1.0f;
        both.scale = 0.52f;
        s.elements.push_back(both);
        scenes.push_back(std::move(s));
    }

    // Title screen: the "Press A" bubble under the logo.
    {
        // Recognised by the title background (kept drawn off-screen by the patch). Welcome amiibo
        // reuses that image in its amiibo camera, which is ruled out by its own textures.
        Scene s{"title", {}, {}, false, 2, {"Title/opng_titl.arc:my_opn_bg_01.bclim"}};
        s.excluded_by = {"camera_amiibo/camera_amiibo.arc:"};
        s.requires_text = true; // the bubble pops in with "Press A", not before
        s.suppressed_after = "title_menu";
        s.suppress_seconds = 1.5f;
        Element press;
        press.id = "title_press";
        press.src = {60.0f, 84.0f, 260.0f, 156.0f};
        // Cropped to the bubble itself (the menu bubble parts grafted into the title layout).
        // (Its pieces are shared with the menu bubble and, in Welcome amiibo, the warning
        // screen; on the title screen they only draw this bubble.)
        press.bounds_of = {"Title/opng_titl.arc:my_csw*", "selWin/cmn_slct_win.arc:my_csw*",
                           "warning/warning.arc:my_csw*"};
        press.bounds_pad = 3.0f;
        press.anchor_x = 0.5f;
        press.pivot_x = 0.5f;
        if (IsWelcomeAmiibo()) {
            // Below the "Welcome amiibo" ribbon, which hangs lower than the original logo.
            press.anchor_y = 0.985f;
            press.pivot_y = 1.0f;
            press.scale = 0.72f;
        } else {
            press.anchor_y = 0.86f;
            press.pivot_y = 0.5f;
            press.scale = 0.95f;
        }
        s.elements.push_back(press);
        scenes.push_back(std::move(s));
    }

    // Title menu (New Save File / Move In / Update Data ...), centred on the main screen.
    {
        // The menu's bubble textures are also grafted into the title screen, so the menu is
        // recognised by its cursor arrow appearing on the title screen.
        Scene s{"title_menu",
                {},
                {},
                false,
                4,
                {"selWin/cmn_slct_win.arc:im_cmn_arrow32_00.bclim",
                 "Title/opng_titl.arc:my_opn_bg_01.bclim"}};
        s.excluded_by = {"camera_amiibo/camera_amiibo.arc:"};
        Element menu;
        menu.id = "title_menu";
        menu.src = {20.0f, 20.0f, 300.0f, 220.0f};
        menu.anchor_x = 0.5f;
        menu.anchor_y = 0.55f;
        menu.pivot_x = 0.5f;
        menu.pivot_y = 0.5f;
        menu.scale = 0.85f;
        s.elements.push_back(menu);
        scenes.push_back(std::move(s));
    }

    // Town map shown with Nope!/Here! buttons (choosing the town on the train): the map sits in
    // the top-right corner above the dialog, the buttons stacked to its left.
    {
        // Any map part (they are shared between the map archives) plus the button art.
        Scene s{"map_pick",
                {"Menu/map_village.arc:", "Menu/map_island.arc:", "Menu/map_lobby.arc:",
                 "Menu/map_exb.arc:"},
                {},
                false,
                3,
                {"common/cmn_btn.arc:my_cbn_btn.bclim"}};
        Element map;
        map.id = "map_pick_map";
        map.src = {63.0f, 17.0f, 258.0f, 201.0f};
        // The two buttons sit in a row under the map, which is centred over them.
        constexpr float MapHeight = (201.0f - 17.0f) * 0.5f;
        constexpr float ButtonScale = 0.5f;
        constexpr float ButtonWidth = 124.0f * ButtonScale;
        constexpr float RowWidth = ButtonWidth * 2.0f + 4.0f;
        map.anchor_x = 0.97f;
        map.anchor_y = 0.03f;
        map.pivot_x = 0.5f;
        map.pivot_y = 0.0f;
        map.offset_x = -RowWidth * 0.5f;
        map.scale = 0.5f;
        s.elements.push_back(map);
        // Buttons in their 3DS order ("Nope!" on the left, "Here!" on the right), the row
        // ending at the anchor.
        Element here;
        here.id = "map_pick_here";
        here.src = {196.0f, 204.0f, 320.0f, 240.0f};
        here.anchor_x = 0.97f;
        here.anchor_y = 0.03f;
        here.pivot_x = 1.0f;
        here.pivot_y = 0.0f;
        here.offset_y = MapHeight + 4.0f;
        here.scale = ButtonScale;
        s.elements.push_back(here);
        Element nope = here;
        nope.id = "map_pick_nope";
        nope.src = {0.0f, 204.0f, 124.0f, 240.0f};
        nope.offset_x = -(ButtonWidth + 4.0f);
        s.elements.push_back(nope);
        scenes.push_back(std::move(s));
    }

    // Welcome amiibo passport, cat tasks page: the menu tabs and the coupon count (top screen)
    // over the game along the top edge, the task list below them across the screen on a dimmed
    // backdrop. Selecting a task (touch or A) opens its details (the top screen's window) over
    // the list; touching anywhere, its close button or B closes them again. Told apart from the
    // other passport pages by the details window on the top screen.
    {
        Scene s{"cat_tasks", {"Menu/menu_tab.arc:"}, {}, false, 1};
        s.requires_top = {"passWin/m_plife.arc:my_plife_TL.bclim"};
        s.instant_out = true;
        s.overlay_on_confirm = true;
        // The list at its 3DS size, standing on the bottom edge (it fills a 4:3 screen below
        // the tabs), on an opaque panel: the details window on the top screen is under it and
        // should not show through. Touching a task also opens its details.
        Element list;
        list.id = "cat_task_list";
        list.src = {0.0f, 58.0f, 320.0f, 240.0f};
        list.anchor_x = 0.5f;
        list.anchor_y = 1.0f;
        list.pivot_x = 0.5f;
        list.pivot_y = 1.0f;
        list.scrim = 1.0f;
        list.action = 1;
        s.elements.push_back(list);
        // Above it, over the game: the passport's page tabs, and the menu tabs hanging from the
        // top edge.
        Element pages;
        pages.id = "cat_task_pages";
        pages.src = {0.0f, 26.0f, 320.0f, 58.0f};
        pages.anchor_x = pages.anchor_y = pages.pivot_x = pages.pivot_y = 0.0f;
        pages.same_transform_as = "cat_task_list";
        s.elements.push_back(pages);
        Element tabs;
        tabs.id = "cat_task_tabs";
        tabs.src = {0.0f, 1.0f, 320.0f, 26.0f};
        tabs.anchor_x = 0.5f;
        tabs.anchor_y = 0.0f;
        tabs.pivot_x = 0.5f;
        tabs.pivot_y = 0.0f;
        s.elements.push_back(tabs);
        // The coupon count pill (top-right of the top screen) dips into the page tabs: its
        // lower edge is brought in front of them.
        Element pill;
        pill.id = "cat_task_coupons";
        pill.from_top = true;
        pill.top_in_place = true;
        pill.src = {280.0f, 29.0f, 398.0f, 35.0f};
        pill.anchor_x = pill.anchor_y = pill.pivot_x = pill.pivot_y = 0.0f;
        pill.key_tolerance = 0.0f;
        pill.touchable = false;
        s.elements.push_back(pill);
        // Details overlay: dims everything (touching it closes the overlay), the details window
        // from the top screen at its 3DS proportions, and a close button on its corner (drawn
        // by the frontend at this element's place).
        constexpr float Shade = 0.55f;
        Element shade;
        shade.id = "cat_task_shade";
        shade.src = {0.0f, 0.0f, 320.0f, 240.0f};
        shade.anchor_x = shade.anchor_y = shade.pivot_x = shade.pivot_y = 0.0f;
        shade.backdrop = true;
        shade.scrim = Shade;
        shade.overlay = true;
        shade.action = 2;
        s.elements.push_back(shade);
        // The window as moved by the layout patch (0.847 of 328x222, top edge at y 52), shown
        // at 0.95 of its 3DS size.
        constexpr float DetailsScale = 0.95f / 0.847f;
        constexpr float DetailsTop = 53.5f;
        constexpr float DetailsHalfWidth = 138.0f, DetailsHalfHeight = 93.25f;
        Element details;
        details.id = "cat_task_details";
        details.from_top = true;
        details.src = {200.0f - DetailsHalfWidth, DetailsTop, 200.0f + DetailsHalfWidth,
                       DetailsTop + 2.0f * DetailsHalfHeight};
        details.anchor_x = 0.5f;
        details.anchor_y = 0.5f;
        details.pivot_x = 0.5f;
        details.pivot_y = 0.5f;
        details.scale = DetailsScale;
        details.key_tolerance = 0.0f;
        details.touchable = false;
        details.overlay = true;
        // Its rounded corners: the crop's corners outside them are dimmed like the backdrop.
        details.icon = IconRoundCorners;
        details.corner_radius = 10.0f;
        details.corner_shade = Shade;
        s.elements.push_back(details);
        Element close;
        close.id = "cat_task_close";
        close.src = {0.0f, 0.0f, 22.0f, 22.0f};
        close.anchor_x = 0.5f;
        close.anchor_y = 0.5f;
        close.pivot_x = 0.5f;
        close.pivot_y = 0.5f;
        close.offset_x = DetailsHalfWidth * DetailsScale - 16.0f; // in the top-right corner
        close.offset_y = -DetailsHalfHeight * DetailsScale + 16.0f;
        close.opacity = 0.0f;
        close.overlay = true;
        close.action = 2;
        close.icon = IconClose;
        s.elements.push_back(close);
        scenes.push_back(std::move(s));
    }

    // Welcome amiibo coupon machine: its menu fills the main screen (its scenery is removed, so
    // the top screen shows around the buttons), with the coupon count pill from the top screen
    // in the top-right corner (layout patch).
    {
        Scene s{"coupons", {"ticket/ticket.arc:"}, {}, false, 3};
        s.instant_out = true;
        Element menu;
        menu.id = "coupon_menu";
        menu.src = {0.0f, 1.0f, 320.0f, 240.0f};
        menu.anchor_x = 0.5f;
        menu.anchor_y = 0.0f;
        menu.pivot_x = 0.5f;
        menu.pivot_y = 0.0f;
        menu.fit = true;
        s.elements.push_back(menu);
        scenes.push_back(std::move(s));
    }

    // Town map shown by a character during a conversation (e.g. Isabelle pointing out Main
    // Street): a smaller picture just above the dialog box, gone once the player reads on.
    {
        // Map parts are shared between the town, island and lobby maps.
        Scene s{"map_show",
                {"Menu/map_village.arc:", "Menu/map_island.arc:", "Menu/map_lobby.arc:",
                 "Menu/map_exb.arc:"},
                {},
                false,
                2};
        s.excluded_by = {"Menu/menu_tab.arc:"}; // the town HUD's own minimap
        s.dismiss_on_confirm = true;
        Element map;
        map.id = "map_show";
        map.src = {0.0f, 0.0f, 320.0f, 224.0f};
        // Everything drawn (place name, map, binder tab): the place name sits further left in
        // Welcome amiibo, so a fixed crop would cut it.
        map.bounds_of = {"*"};
        map.bounds_pad = 2.0f;
        map.follow_art = true; // sits on the dialog box, which is top-screen art
        map.anchor_x = 0.5f;
        map.anchor_y = 0.485f; // a little above the dialog box's top edge
        map.pivot_x = 0.5f;
        map.pivot_y = 1.0f;
        map.scale = 0.5f;
        map.touchable = false;
        s.elements.push_back(map);
        scenes.push_back(std::move(s));
    }

    // Software keyboard (names, letters, signs, chat): full-screen takeover until confirmed.
    {
        Scene s{"keyboard",
                {"Swkbd/swkbd_qwerty.arc:", "Swkbd/swkbd_kana.arc:", "Swkbd/swkbd_grid.arc:",
                 "Swkbd/swkbd_cell.arc:"},
                {},
                false,
                10};
        s.instant_out = true;
        s.requires_onscreen = true;
        Element backdrop;
        backdrop.id = "keyboard_backdrop";
        backdrop.backdrop = true;
        backdrop.scrim = 0.55f;
        backdrop.touchable = false;
        s.elements.push_back(backdrop);
        Element keyboard;
        keyboard.id = "keyboard";
        keyboard.src = {0.0f, 1.0f, 320.0f, 240.0f};
        keyboard.anchor_x = 0.5f;
        keyboard.anchor_y = 0.5f;
        keyboard.pivot_x = 0.5f;
        keyboard.pivot_y = 0.5f;
        keyboard.fit = true;
        keyboard.scrim = 0.0f; // the backdrop already dims the game
        s.elements.push_back(keyboard);
        scenes.push_back(std::move(s));
    }

    // Pockets, designs, encyclopedia and friends (opened from the town HUD's tabs): the whole
    // bottom screen takes over the main screen until closed, like the keyboard.
    {
        Scene s{"tab_screen", {"Menu/menu_tab.arc:"}, {}, false, 0};
        s.instant_out = true;
        Element backdrop;
        backdrop.id = "tab_backdrop";
        backdrop.backdrop = true;
        backdrop.scrim = 0.45f;
        backdrop.touchable = false;
        s.elements.push_back(backdrop);
        Element screen;
        screen.id = "tab_screen";
        screen.src = {0.0f, 1.0f, 320.0f, 240.0f};
        screen.anchor_x = 0.5f;
        screen.anchor_y = 0.5f;
        screen.pivot_x = 0.5f;
        screen.pivot_y = 0.5f;
        screen.fit = true;
        s.elements.push_back(screen);
        scenes.push_back(std::move(s));
    }

    // Town HUD (bottom screen while walking around): menu tabs hanging from the top-left edge,
    // the minimap in the bottom-right corner and the chat button beside it. The game's own
    // date/time plate already sits bottom-left on the main screen. Shown only while standing
    // still and not talking to anyone; tapping an element works like on the touch screen.
    {
        // The chat button only exists while walking around; the tabs alone also frame the
        // pockets, designs, encyclopedia and friends screens, which use the whole-screen view.
        Scene s{"town_hud", {"Menu/menu_tab.arc:"}, {}, true, 1,
                {"Menu/menu_tab.arc:my_men_btn_00.bclim"}};
        s.hidden_when_top = {"msgWin/msg_win.arc:"};
        s.shown_with_top = {"Menu/time_bel_win.arc:ny_tim_clkbase.bclim"}; // the clock plate
        Element tabs;
        tabs.id = "hud_tabs";
        tabs.src = {0.0f, 0.0f, 210.0f, 25.0f}; // bag, designs, encyclopedia, friends
        tabs.anchor_x = 0.0f;
        tabs.anchor_y = 0.0f;
        tabs.pivot_x = 0.0f;
        tabs.pivot_y = 0.0f;
        tabs.offset_x = 4.0f;
        s.elements.push_back(tabs);
        // The map grows to the left when its list is opened (the arrow on its right edge).
        Element map;
        map.id = "hud_map";
        map.src = {64.0f, 26.0f, 280.0f, 214.0f}; // map and its binder tab
        map.bounds_of = {"Menu/map_village.arc:tc_map_list_win*", "Menu/map_island.arc:tc_map_list_win*",
                         "Menu/map_lobby.arc:tc_map_list_win*", "Menu/map_exb.arc:tc_map_list_win*",
                         "Menu/map_village.arc:tc_map_listwin_arrow.bclim"};
        map.bounds_clamp = {0.0f, 26.0f, 320.0f, 240.0f}; // the menu tabs are above
        map.anchor_x = 1.0f;
        map.anchor_y = 1.0f;
        map.pivot_x = 1.0f;
        map.pivot_y = 1.0f;
        map.offset_x = -4.0f;
        map.offset_y = -4.0f;
        map.scale = 0.42f;
        s.elements.push_back(map);
        // The map's orange tag sticks out above it, next to the menu tabs.
        Element tag;
        tag.id = "hud_map_tag";
        tag.src = {264.0f, 13.0f, 296.0f, 26.0f};
        tag.bounds_of = {"Menu/map_village.arc:tc_map_listwin_tag.bclim"};
        tag.bounds_clamp = {0.0f, 0.0f, 320.0f, 26.0f};
        tag.same_transform_as = "hud_map";
        s.elements.push_back(tag);
        // Chat button: on the bottom edge, just left of the map (also when the map is opened).
        Element chat;
        chat.id = "hud_chat";
        chat.src = {104.0f, 215.0f, 216.0f, 240.0f};
        chat.left_of = "hud_map";
        chat.anchor_x = 1.0f;
        chat.anchor_y = 1.0f;
        chat.pivot_x = 1.0f;
        chat.pivot_y = 1.0f;
        chat.offset_x = -6.0f;
        chat.scale = 0.6f;
        s.elements.push_back(chat);
        // Welcome amiibo only: the amiibo "Call" button, just above the minimap (right-aligned
        // with it). Its Miiverse post button is removed by the layout patch.
        Element call;
        call.id = "hud_call";
        call.src = {254.0f, 162.0f, 318.0f, 230.0f};
        call.bounds_of = {"Menu/menu_tab.arc:my_men_call_00.bclim"};
        call.bounds_pad = 2.0f;
        call.requires_bounds = true;
        call.above = "hud_map";
        call.anchor_x = 1.0f;
        call.pivot_x = 1.0f;
        call.pivot_y = 1.0f;
        call.offset_x = -4.0f;
        call.offset_y = -2.0f;
        call.scale = 0.5f;
        s.elements.push_back(call);
        // Leaf's button for the big map, in the minimap's top-left corner. The big map is the
        // same live map, larger in the middle of the screen (the game is not paused; it stays
        // up while walking), with a button to go back; the other HUD parts step aside.
        Element expand;
        expand.id = "hud_map_expand";
        expand.src = {66.0f, 28.0f, 110.0f, 72.0f};
        expand.anchor_x = expand.anchor_y = expand.pivot_x = expand.pivot_y = 0.0f;
        expand.same_transform_as = "hud_map";
        expand.opacity = 0.0f;
        expand.action = 3;
        expand.icon = IconExpand;
        s.elements.push_back(expand);
        s.overlay_replaces = true;
        Element shade;
        shade.id = "hud_big_map_shade";
        shade.src = {0.0f, 0.0f, 320.0f, 240.0f};
        shade.anchor_x = shade.anchor_y = shade.pivot_x = shade.pivot_y = 0.0f;
        shade.backdrop = true;
        shade.scrim = 0.35f;
        shade.overlay = true;
        shade.action = 2; // touching beside the map goes back
        s.elements.push_back(shade);
        Element big = map;
        big.id = "hud_big_map";
        big.anchor_x = 0.5f;
        big.anchor_y = 0.53f; // a little low: its tag sticks out above
        big.pivot_x = 0.5f;
        big.pivot_y = 0.5f;
        big.offset_x = 0.0f;
        big.offset_y = 0.0f;
        big.scale = 1.1f;
        big.overlay = true;
        s.elements.push_back(big);
        Element big_tag = tag;
        big_tag.id = "hud_big_map_tag";
        big_tag.same_transform_as = "hud_big_map";
        big_tag.overlay = true;
        s.elements.push_back(big_tag);
        Element collapse;
        collapse.id = "hud_big_map_collapse";
        collapse.src = {64.0f, 26.0f, 90.0f, 52.0f};
        collapse.anchor_x = collapse.anchor_y = collapse.pivot_x = collapse.pivot_y = 0.0f;
        collapse.same_transform_as = "hud_big_map";
        collapse.opacity = 0.0f;
        collapse.overlay = true;
        collapse.action = 2;
        collapse.icon = IconCollapse;
        s.elements.push_back(collapse);
        scenes.push_back(std::move(s));
    }
    return scenes;
}

// Shown when the bottom screen has game UI that no scene above covers, so nothing is ever
// unreachable: the whole bottom screen over the game, with its (keyed) background removed.
Scene FallbackScene() {
    Scene s{"fallback", {}, {}, false, -1};
    // Menus without a layout of their own (mailbox, shops, ...): as large as the main screen
    // allows, hanging from its top edge like the pockets.
    Element all;
    all.id = "bottom_screen";
    all.src = {0.0f, 1.0f, 320.0f, 240.0f};
    all.anchor_x = 0.5f;
    all.anchor_y = 0.0f;
    all.pivot_x = 0.5f;
    all.pivot_y = 0.0f;
    all.fit = true;
    s.elements.push_back(all);
    return s;
}

struct SceneState {
    Scene scene;
    std::unordered_set<u64> keys;
    std::unordered_set<u64> hide_top_keys; ///< top-screen textures that hide the scene
    std::unordered_set<u64> excluded_keys; ///< bottom-screen textures that rule the scene out
    std::unordered_set<u64> show_top_keys; ///< top-screen textures the scene needs to show
    std::unordered_set<u64> required_top_keys; ///< top-screen textures the scene needs at all
    std::vector<std::unordered_set<u64>> required; ///< one key set per requirement
    std::vector<std::unordered_set<u64>> bounds_keys; ///< per element (Element::bounds_of)
    float alpha = 0.0f;
    float since_shown = 1e9f; ///< seconds since this scene was last on screen
    bool was_present = false;
    u32 confirm_mark = 0; ///< A presses counted when the scene came up
    bool dismissed = false;
};

std::mutex g_mutex;
std::vector<SceneState> g_scenes;
SceneState g_fallback{FallbackScene(), {}, {}, {}, {}, {}, {}, {}, 0.0f, 1e9f};
std::unordered_set<u64> g_ui_keys; ///< layout textures that indicate real UI
std::unordered_set<u64> g_layout_keys; ///< every layout texture
std::unordered_map<u64, std::string> g_key_names; ///< for the diagnostic log
std::string g_last_logged; ///< scenes shown when the log last ran
float g_fallback_stable = 0.0f;   ///< seconds the fallback condition has held
std::unordered_map<u64, std::vector<Layout::ScreenRegion>> g_last_regions; ///< per window size
Clock::time_point g_last_tick{};
std::atomic<bool> g_moving{false};
std::atomic<u32> g_confirms{0}; ///< A presses so far
std::atomic<bool> g_overlay_open{false};
std::atomic<bool> g_overlay_available{false}; ///< a scene with an overlay is on screen
std::atomic<bool> g_overlay_on_confirm{false}; ///< ... that A opens
std::atomic<Clock::rep> g_last_movement{0}; ///< Clock ticks since epoch

bool MatchesPattern(const std::string& name, const std::string& pattern) {
    if (pattern.ends_with("*"))
        return name.starts_with(std::string_view(pattern).substr(0, pattern.size() - 1));
    return pattern.ends_with(":") ? name.starts_with(pattern) : name == pattern;
}

Layout::ScreenRegion PlaceElement(const Element& e, const Common::Rectangle<u32>& main,
                                  float alpha, const Layout::ScreenRegion* same_transform = nullptr,
                                  const Layout::ScreenRegion* left_of = nullptr,
                                  const Layout::ScreenRegion* above = nullptr) {
    const float unit = main.GetHeight() / NativeTopHeight;
    Layout::ScreenRegion region;
    const float src_width = e.from_top ? 400.0f : 320.0f;
    region.src = {e.src.left / src_width, e.src.top / 240.0f, e.src.right / src_width,
                  e.src.bottom / 240.0f};
    region.from_top = e.from_top;
    region.action = e.action;
    region.icon = e.icon;
    region.corner_shade = e.corner_shade;
    region.opacity = e.opacity * alpha;
    region.touchable = e.touchable && !e.from_top && alpha > 0.6f;
    region.key_tolerance = e.key_tolerance;
    region.key_softness = e.key_softness;
    region.scrim = e.scrim;
    if (e.from_top && e.top_in_place) {
        // The main screen shows the top screen without its first row (TopScreenCrop).
        const auto map_x = [&](float v) { return main.left + v / 400.0f * main.GetWidth(); };
        const auto map_y = [&](float v) {
            return main.top + std::max(0.0f, v - 1.0f) / 239.0f * main.GetHeight();
        };
        region.dst = {static_cast<u32>(std::round(map_x(e.src.left))),
                      static_cast<u32>(std::round(map_y(e.src.top))),
                      static_cast<u32>(std::round(map_x(e.src.right))),
                      static_cast<u32>(std::round(map_y(e.src.bottom)))};
        return region;
    }
    if (same_transform) {
        const auto& r = *same_transform;
        const float kx = r.dst.GetWidth() / std::max(1e-6f, r.src.right - r.src.left);
        const float ky = r.dst.GetHeight() / std::max(1e-6f, r.src.bottom - r.src.top);
        const auto map_x = [&](float v) { return r.dst.left + (v - r.src.left) * kx; };
        const auto map_y = [&](float v) { return r.dst.top + (v - r.src.top) * ky; };
        region.dst = {static_cast<u32>(std::max(0.0f, std::round(map_x(region.src.left)))),
                      static_cast<u32>(std::max(0.0f, std::round(map_y(region.src.top)))),
                      static_cast<u32>(std::max(0.0f, std::round(map_x(region.src.right)))),
                      static_cast<u32>(std::max(0.0f, std::round(map_y(region.src.bottom))))};
        return region;
    }
    float anchor_x = e.anchor_x;
    float anchor_y = e.anchor_y;
    if (e.follow_art) {
        const auto [art_x, art_y] = ArtScale();
        anchor_x = 0.5f + (anchor_x - 0.5f) * art_x;
        anchor_y = 1.0f - (1.0f - anchor_y) * art_y;
    }
    const float src_w = e.src.right - e.src.left;
    const float src_h = e.src.bottom - e.src.top;
    float scale = e.scale * unit;
    if (e.fit)
        scale = std::min(main.GetWidth() / src_w, main.GetHeight() / src_h);
    // Gentle "pop" while fading in or out.
    scale *= 0.94f + 0.06f * alpha;
    const float w = src_w * scale;
    const float h = src_h * scale;
    const float x = (left_of ? static_cast<float>(left_of->dst.left)
                             : main.left + anchor_x * main.GetWidth()) +
                    e.offset_x * unit - e.pivot_x * w;
    const float y = (above ? static_cast<float>(above->dst.top)
                           : main.top + anchor_y * main.GetHeight()) +
                    e.offset_y * unit - e.pivot_y * h;

    region.dst = {static_cast<u32>(std::max(0.0f, std::round(x))),
                  static_cast<u32>(std::max(0.0f, std::round(y))),
                  static_cast<u32>(std::max(0.0f, std::round(x + w))),
                  static_cast<u32>(std::max(0.0f, std::round(y + h)))};
    region.corner_radius = e.corner_radius * region.dst.GetWidth() / std::max(1.0f, src_w);
    if (e.backdrop) {
        region.dst = main;
        // Only a close-the-overlay backdrop takes touches (anywhere on it).
        region.touchable = e.action == 2 && alpha > 0.6f;
        region.key_tolerance = -1.0f; // tells the present shader to draw a solid scrim
    }
    return region;
}

} // namespace

void Initialize(Loader::AppLoader& loader) {
    const auto textures = UiPatcher::BuildTextureKeys(loader);
    std::unordered_map<u64, std::vector<const std::string*>> names_by_key;
    for (const auto& [name, key] : textures)
        names_by_key[key].push_back(&name);

    std::scoped_lock lock{g_mutex};
    // Textures that count as "real UI" for the fallback: not the wallpaper, not screen
    // transitions, and not generic parts shared by many layouts (plain fills, arrows, ...).
    g_ui_keys.clear();
    g_layout_keys.clear();
    g_key_names.clear();
    for (const auto& [name, key] : textures)
        g_key_names.try_emplace(key, name);
    for (const auto& [key, names] : names_by_key) {
        g_layout_keys.insert(key);
        std::unordered_set<std::string> archives;
        bool excluded = false;
        for (const auto* name : names) {
            archives.insert(name->substr(0, name->find(':')));
            excluded |= name->starts_with("Menu/mvmnt_undscrn.arc:") ||
                        name->starts_with("Wipe/") || name->starts_with("common/save_wait") ||
                        name->starts_with("common/cmn_dialog");
        }
        if (!excluded && archives.size() < 3)
            g_ui_keys.insert(key);
    }
    // The coupon machine draws its messages once, then only sends the bottom screen out again.
    std::unordered_set<u64> hold_keys;
    for (const auto& [key, names] : names_by_key) {
        if (std::all_of(names.begin(), names.end(),
                        [](const auto* n) { return n->starts_with("ticket/ticket.arc:"); }))
            hold_keys.insert(key);
    }
    UiProbe::SetHoldTopKeys(std::move(hold_keys));
    g_fallback.alpha = 0.0f;
    g_scenes.clear();
    for (auto& scene : BuildScenes()) {
        SceneState state{std::move(scene), {}, {}, {}, {}, {}, {}, {}, 0.0f, 1e9f};
        for (const auto& element : state.scene.elements) {
            const auto matches = [&](const std::string& name) {
                return std::any_of(element.bounds_of.begin(), element.bounds_of.end(),
                                   [&](const auto& p) { return MatchesPattern(name, p); });
            };
            std::unordered_set<u64> keys;
            for (const auto& [key, names] : names_by_key) {
                if (element.bounds_exclusive &&
                    std::all_of(names.begin(), names.end(),
                                [&](const auto* n) { return matches(*n); }))
                    keys.insert(key);
            }
            if (keys.empty()) {
                for (const auto& [name, key] : textures) {
                    if (matches(name))
                        keys.insert(key);
                }
            }
            state.bounds_keys.push_back(std::move(keys));
        }
        for (const auto& [key, names] : names_by_key) {
            if (state.scene.hidden_when_top.empty())
                break;
            const bool exclusive = std::all_of(names.begin(), names.end(), [&](const auto* n) {
                return std::any_of(state.scene.hidden_when_top.begin(),
                                   state.scene.hidden_when_top.end(),
                                   [&](const auto& p) { return MatchesPattern(*n, p); });
            });
            if (exclusive)
                state.hide_top_keys.insert(key);
        }
        for (const auto& [key, names] : names_by_key) {
            if (state.scene.requires_top.empty())
                break;
            const bool exclusive = std::all_of(names.begin(), names.end(), [&](const auto* n) {
                return std::any_of(state.scene.requires_top.begin(),
                                   state.scene.requires_top.end(),
                                   [&](const auto& p) { return MatchesPattern(*n, p); });
            });
            if (exclusive)
                state.required_top_keys.insert(key);
        }
        for (const auto& [key, names] : names_by_key) {
            if (state.scene.shown_with_top.empty())
                break;
            const bool exclusive = std::all_of(names.begin(), names.end(), [&](const auto* n) {
                return std::any_of(state.scene.shown_with_top.begin(),
                                   state.scene.shown_with_top.end(),
                                   [&](const auto& p) { return MatchesPattern(*n, p); });
            });
            if (exclusive)
                state.show_top_keys.insert(key);
        }
        for (const auto& [key, names] : names_by_key) {
            if (state.scene.excluded_by.empty())
                break;
            const bool exclusive = std::all_of(names.begin(), names.end(), [&](const auto* n) {
                return std::any_of(state.scene.excluded_by.begin(), state.scene.excluded_by.end(),
                                   [&](const auto& p) { return MatchesPattern(*n, p); });
            });
            if (exclusive)
                state.excluded_keys.insert(key);
        }
        for (const auto& pattern : state.scene.requires_all) {
            // Prefer images only found under the pattern: a generic image that the pattern's
            // archive shares with others (e.g. the pockets screen) must not satisfy it.
            std::unordered_set<u64> keys;
            for (const auto& [key, names] : names_by_key) {
                if (std::all_of(names.begin(), names.end(),
                                [&](const auto* n) { return MatchesPattern(*n, pattern); }))
                    keys.insert(key);
            }
            if (keys.empty()) {
                for (const auto& [name, key] : textures) {
                    if (MatchesPattern(name, pattern))
                        keys.insert(key);
                }
            }
            if (keys.empty())
                LOG_WARNING(Core, "Leaf: scene {} needs unknown texture {}", state.scene.id,
                            pattern);
            state.required.push_back(std::move(keys));
        }
        // Only textures that belong exclusively to the scene's archives identify it; shared
        // images (arrows, common window parts) appear in many contexts.
        for (const auto& [key, names] : names_by_key) {
            const bool exclusive = std::all_of(names.begin(), names.end(), [&](const auto* n) {
                return std::any_of(state.scene.textures.begin(), state.scene.textures.end(),
                                   [&](const auto& p) { return MatchesPattern(*n, p); });
            });
            if (exclusive)
                state.keys.insert(key);
        }
        LOG_INFO(Core, "Leaf: scene {} identified by {} textures", state.scene.id,
                 state.keys.size());
        g_scenes.push_back(std::move(state));
    }
    g_last_tick = Clock::now();
}

void Shutdown() {
    std::scoped_lock lock{g_mutex};
    g_scenes.clear();
    g_last_regions.clear();
}

void NotifyConfirm() {
    g_confirms++;
    if (g_overlay_available && g_overlay_on_confirm)
        g_overlay_open = true;
}

void SetOverlayOpen(bool open) {
    g_overlay_open = open && g_overlay_available;
}

bool OverlayOpen() {
    return g_overlay_open;
}

std::vector<Button> Buttons(u32 width, u32 height) {
    std::scoped_lock lock{g_mutex};
    std::vector<Button> buttons;
    const auto it = g_last_regions.find((static_cast<u64>(width) << 32) | height);
    if (it == g_last_regions.end())
        return buttons;
    for (const auto& region : it->second) {
        const bool shown = region.icon == IconRoundCorners ? region.opacity > 0.6f
                                                           : region.touchable;
        if (region.icon != IconNone && shown && region.dst.GetWidth() > 0)
            buttons.push_back(
                {region.icon, region.dst, region.corner_radius, region.corner_shade});
    }
    return buttons;
}

void SetMovementInput(bool moving) {
    if (moving)
        g_last_movement = Clock::now().time_since_epoch().count();
    g_moving = moving;
}

std::vector<Layout::ScreenRegion> FrameRegions(const Layout::FramebufferLayout& layout,
                                               float forced_dt) {
    const auto visible = UiProbe::LastFrameKeys();
    const auto measured = UiProbe::LastFrameBounds();
    const auto top_visible = UiProbe::LastFrameTopKeys();
    const auto now = Clock::now();

    std::scoped_lock lock{g_mutex};
    const float dt =
        forced_dt >= 0.0f
            ? forced_dt
            : std::clamp(std::chrono::duration<float>(now - g_last_tick).count(), 0.0f, 0.1f);
    g_last_tick = now;
    const bool recently_moved =
        g_moving ||
        std::chrono::duration<float>(now - Clock::time_point(Clock::duration(g_last_movement)))
                .count() < StillSecondsBeforeHud;

    // Which scenes does the bottom screen show right now?
    int top_priority = std::numeric_limits<int>::min();
    std::vector<bool> present(g_scenes.size(), false);
    bool any_claimed = false; ///< a scene owns the bottom screen, even if it waits for its text
    for (std::size_t i = 0; i < g_scenes.size(); i++) {
        const auto& scene = g_scenes[i];
        const auto on_screen = [&](const std::unordered_set<u64>& keys) {
            return std::any_of(visible.begin(), visible.end(),
                               [&](u64 key) { return keys.contains(key); });
        };
        const bool any_own = scene.scene.textures.empty() ||
                             std::any_of(visible.begin(), visible.end(),
                                         [&](u64 key) { return scene.keys.contains(key); });
        const bool all_required = std::all_of(scene.required.begin(), scene.required.end(),
                                              on_screen);
        const bool text_ok =
            !scene.scene.requires_text ||
            std::any_of(visible.begin(), visible.end(),
                        [](u64 key) { return key != 0 && !g_layout_keys.contains(key); });
        bool excluded = on_screen(scene.excluded_keys);
        // (A requirement this game version has no texture for can never be met.)
        if (!scene.scene.requires_top.empty() &&
            std::none_of(top_visible.begin(), top_visible.end(),
                         [&](u64 key) { return scene.required_top_keys.contains(key); }))
            excluded = true;
        if (scene.scene.requires_onscreen && !measured.empty()) {
            // Any of the scene's own textures measured inside the screen? (Without
            // measurements yet, e.g. on the first frame, the scene is not held back.)
            bool any_measured = false, inside = false;
            for (const auto& [key, rect] : measured) {
                if (!scene.keys.contains(key))
                    continue;
                any_measured = true;
                inside |= rect.right > 1.0f && rect.left < 319.0f && rect.bottom > 1.0f &&
                          rect.top < 239.0f;
            }
            excluded |= any_measured && !inside;
        }
        const bool claimed = any_own && all_required && !excluded &&
                             (!scene.scene.textures.empty() || !scene.required.empty());
        any_claimed |= claimed;
        present[i] = claimed && text_ok;
        if (present[i])
            top_priority = std::max(top_priority, g_scenes[i].scene.priority);
    }

    // Fallback when known UI is on the bottom screen but no scene claims it. It must be stable
    // for a moment so loading screens and transitions never flash on the main screen.
    const bool any_scene = any_claimed;
    const bool any_ui = std::any_of(visible.begin(), visible.end(),
                                    [](u64 key) { return g_ui_keys.contains(key); });
    {
        const bool candidate = any_ui && !any_scene;
        g_fallback_stable = candidate ? g_fallback_stable + dt : 0.0f;
        const bool show = candidate && g_fallback_stable >= FallbackSettleSeconds;
        const float step = dt / (show ? FadeInSeconds : FadeOutSeconds);
        g_fallback.alpha = show ? std::clamp(g_fallback.alpha + step, 0.0f, 1.0f) : 0.0f;
    }

    // Track how recently each scene was up, for scenes suppressed after another one.
    for (std::size_t i = 0; i < g_scenes.size(); i++)
        g_scenes[i].since_shown = present[i] ? 0.0f : g_scenes[i].since_shown + dt;
    const auto suppressed = [&](const SceneState& state) {
        if (state.scene.suppressed_after.empty())
            return false;
        return std::any_of(g_scenes.begin(), g_scenes.end(), [&](const SceneState& other) {
            return other.scene.id == state.scene.suppressed_after &&
                   other.since_shown < state.scene.suppress_seconds;
        });
    };

    std::vector<bool> showing(g_scenes.size(), false);
    bool any_overlay = false, any_overlay_on_confirm = false;
    std::vector<Layout::ScreenRegion> regions;
    if (g_fallback.alpha > 0.0f) {
        const float a = g_fallback.alpha;
        for (const auto& element : g_fallback.scene.elements)
            regions.push_back(PlaceElement(element, layout.top_screen, a * a * (3.0f - 2.0f * a)));
    }
    for (std::size_t i = 0; i < g_scenes.size(); i++) {
        auto& state = g_scenes[i];
        bool show = present[i] && state.scene.priority == top_priority && !suppressed(state);
        if (show && !state.hide_top_keys.empty() &&
            std::any_of(top_visible.begin(), top_visible.end(),
                        [&](u64 key) { return state.hide_top_keys.contains(key); }))
            show = false;
        // An open overlay that replaces the scene (e.g. the big map) stays while walking.
        const bool keep_for_overlay = state.scene.overlay_replaces && g_overlay_open &&
                                      state.alpha > 0.0f;
        if (keep_for_overlay) {
            // (not hidden by walking or by the game hiding its clock)
        } else if (!state.show_top_keys.empty()) {
            // Follows the game's own top-screen element (shown and hidden by the game); the
            // player moving still hides the scene immediately.
            if (g_moving ||
                std::none_of(top_visible.begin(), top_visible.end(),
                             [&](u64 key) { return state.show_top_keys.contains(key); }))
                show = false;
        } else if (state.scene.hide_while_moving && recently_moved) {
            show = false;
        }
        if (state.scene.dismiss_on_confirm) {
            const u32 confirms = g_confirms.load();
            if (present[i] && !state.was_present)
                state.confirm_mark = confirms;
            if (present[i] && confirms != state.confirm_mark)
                state.dismissed = true;
            if (!present[i] && state.since_shown > 0.5f)
                state.dismissed = false;
            state.was_present = present[i];
            if (state.dismissed)
                show = false;
        }
        const float step = dt / (show ? FadeInSeconds : FadeOutSeconds);
        state.alpha = std::clamp(state.alpha + (show ? step : -step), 0.0f, 1.0f);
        if (!show && state.scene.instant_out)
            state.alpha = 0.0f;
        showing[i] = show || state.alpha > 0.0f;
        if (state.alpha <= 0.0f)
            continue;
        // Smoothstep easing for the fade.
        const float eased = state.alpha * state.alpha * (3.0f - 2.0f * state.alpha);
        const std::size_t first = regions.size();
        const auto placed = [&](const std::string& id) -> const Layout::ScreenRegion* {
            for (std::size_t j = 0; j < state.scene.elements.size() && first + j < regions.size();
                 j++) {
                if (state.scene.elements[j].id == id)
                    // A skipped element (empty placeholder) is not a reference point.
                    return regions[first + j].dst.GetWidth() > 0 ? &regions[first + j] : nullptr;
            }
            return nullptr;
        };
        const bool overlay_open = g_overlay_open.load();
        for (std::size_t j = 0; j < state.scene.elements.size(); j++) {
            Element element = state.scene.elements[j];
            if (element.overlay) {
                any_overlay = true;
                any_overlay_on_confirm |= state.scene.overlay_on_confirm;
                if (!overlay_open) {
                    regions.push_back({}); // keeps element indices aligned; draws nothing
                    continue;
                }
            } else if (overlay_open) {
                if (state.scene.overlay_replaces) {
                    regions.push_back({});
                    continue;
                }
                element.touchable = false; // only the overlay takes touches while it is open
            }
            const auto& keys = state.bounds_keys[j];
            if (!keys.empty()) {
                float l = 1e9f, t = 1e9f, r = -1e9f, b = -1e9f;
                for (const auto& [key, rect] : measured) {
                    if (!keys.contains(key))
                        continue;
                    l = std::min(l, rect.left);
                    t = std::min(t, rect.top);
                    r = std::max(r, rect.right);
                    b = std::max(b, rect.bottom);
                }
                const auto& c = element.bounds_clamp;
                l = std::max(c.left, l - element.bounds_pad);
                t = std::max(c.top, t - element.bounds_pad);
                r = std::min(c.right, r + element.bounds_pad);
                b = std::min(c.bottom, b + element.bounds_pad);
                if (r - l >= 4.0f && b - t >= 4.0f)
                    element.src = {l, t, r, b};
                else if (element.requires_bounds) {
                    regions.push_back({}); // keeps element indices aligned; draws nothing
                    continue;
                }
            }
            // Copy the reference: `regions` may reallocate when this element is added.
            std::optional<Layout::ScreenRegion> same, beside, over;
            if (!element.same_transform_as.empty())
                if (const auto* p = placed(element.same_transform_as))
                    same = *p;
            if (!element.left_of.empty())
                if (const auto* p = placed(element.left_of))
                    beside = *p;
            if (!element.above.empty())
                if (const auto* p = placed(element.above))
                    over = *p;
            regions.push_back(PlaceElement(element, layout.top_screen, eased,
                                           same ? &*same : nullptr, beside ? &*beside : nullptr,
                                           over ? &*over : nullptr));
        }
    }
    // An overlay only exists while its scene is on screen.
    g_overlay_available = any_overlay;
    g_overlay_on_confirm = any_overlay_on_confirm;
    if (!any_overlay)
        g_overlay_open = false;
    // Diagnostics: log what is detected whenever the shown scenes change, so new screens and
    // misdetections can be identified from a device log.
    {
        std::string shown = g_fallback.alpha > 0.0f ? "fallback" : "";
        for (std::size_t i = 0; i < g_scenes.size(); i++) {
            if (showing[i])
                shown += (shown.empty() ? "" : ",") + g_scenes[i].scene.id;
        }
        if (shown != g_last_logged) {
            g_last_logged = shown;
            std::string names;
            int listed = 0, unknown = 0;
            for (const u64 key : visible) {
                const auto it = g_key_names.find(key);
                if (it == g_key_names.end()) {
                    unknown++;
                } else if (listed++ < 14) {
                    names += (names.empty() ? "" : " ") + it->second;
                }
            }
            LOG_INFO(Core, "Leaf UI: [{}] bottom: {}{}", shown.empty() ? "-" : shown, names,
                     unknown ? fmt::format(" (+{} other)", unknown) : "");
            LOG_INFO(Core, "Leaf UI xfers:{}", UiProbe::RecentTransfers());
        }
    }
    // Measure draws only while a scene that sizes elements by them is on screen (it costs a
    // CPU vertex-shader pass per UI draw, e.g. not while walking with the town HUD hidden).
    bool want_bounds = false;
    for (std::size_t i = 0; i < g_scenes.size(); i++) {
        const bool own_textures_drawn = std::any_of(
            visible.begin(), visible.end(), [&](u64 key) { return g_scenes[i].keys.contains(key); });
        want_bounds |= g_scenes[i].scene.requires_onscreen && own_textures_drawn;
        if (showing[i])
            for (const auto& keys : g_scenes[i].bounds_keys)
                want_bounds |= !keys.empty();
    }
    UiProbe::SetWantBounds(want_bounds);
    g_last_regions[(static_cast<u64>(layout.width) << 32) | layout.height] = regions;
    return regions;
}

std::vector<Layout::ScreenRegion> LastRegions(u32 width, u32 height) {
    std::scoped_lock lock{g_mutex};
    const auto it = g_last_regions.find((static_cast<u64>(width) << 32) | height);
    return it != g_last_regions.end() ? it->second : std::vector<Layout::ScreenRegion>{};
}

bool BottomToWindow(u32 width, u32 height, float bottom_x, float bottom_y, float& window_x,
                    float& window_y) {
    const float nx = bottom_x / 320.0f, ny = bottom_y / 240.0f;
    for (const auto& r : LastRegions(width, height)) {
        if (nx < r.src.left || nx > r.src.right || ny < r.src.top || ny > r.src.bottom)
            continue;
        const float fx = (nx - r.src.left) / (r.src.right - r.src.left);
        const float fy = (ny - r.src.top) / (r.src.bottom - r.src.top);
        window_x = r.dst.left + fx * r.dst.GetWidth();
        window_y = r.dst.top + fy * r.dst.GetHeight();
        return true;
    }
    return false;
}

std::string TextureName(u64 key) {
    std::scoped_lock lock{g_mutex};
    const auto it = g_key_names.find(key);
    return it != g_key_names.end() ? it->second : std::string{};
}

bool SceneVisible(const std::string& id) {
    std::scoped_lock lock{g_mutex};
    return std::any_of(g_scenes.begin(), g_scenes.end(), [&](const SceneState& s) {
        return s.scene.id == id && s.alpha > 0.5f;
    });
}

std::vector<std::string> ActiveScenes() {
    std::scoped_lock lock{g_mutex};
    std::vector<std::string> ids;
    if (g_fallback.alpha > 0.0f)
        ids.push_back(g_fallback.scene.id);
    for (const auto& state : g_scenes) {
        if (state.alpha > 0.0f)
            ids.push_back(state.scene.id);
    }
    return ids;
}

} // namespace Leaf::Ui
