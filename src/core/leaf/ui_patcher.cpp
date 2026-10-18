// Copyright 2026 Poly
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <sstream>
#include <fmt/format.h>
#include "common/file_util.h"
#include "common/logging/log.h"
#include "core/file_sys/romfs_reader.h"
#include "core/leaf/layout_patch.h"
#include "core/leaf/leaf.h"
#include "core/leaf/ui_patcher.h"
#include "core/leaf/ui_probe.h"
#include "core/loader/loader.h"

namespace Leaf::UiPatcher {

namespace {

using namespace LayoutPatch;

enum class PaneOpKind { Hide, Alpha, Translate, Scale, Size, WhiteVertexColors };

struct PaneOp {
    const char* layout; ///< file inside the archive, e.g. "blyt/slct_win.bclyt"
    const char* pane;
    PaneOpKind kind;
    float x = 0.0f;
    float y = 0.0f;
};

struct Graft {
    const char* source_arc; ///< RomFS path of the archive the window pane comes from
    GraftWindowParams params;
};

/// Top-screen layout whose 2D art is rescaled to keep its proportions at 16:9 / 4:3.
struct AspectFix {
    const char* layout;
    float pivot_x; ///< scaling centre in layout units (400x240, origin at the centre, y up)
    float pivot_y;
};

/// Moves a top-screen HUD part so its visible bottom edge ends up `margin` pixels above the
/// main screen's bottom edge, whatever the aspect scaling (done around `pivot_y`) does to it.
struct BottomAlign {
    const char* layout;
    const char* parent;   ///< its children are moved (the parent itself may be animated)
    float visible_bottom; ///< in layout units (y up, 0 = centre, -120 = bottom edge)
    float pivot_y;        ///< pivot of the layout's AspectFix
    float margin;
};

struct RescaleFix {
    const char* layout;
    float scale;
    float pivot_x; ///< layout units (y up, 0 = centre)
    float pivot_y;
};

struct ArcPatch {
    const char* path;           ///< RomFS path of the archive
    bool key_all_textures;      ///< replace every texture with the transparent key colour
    std::vector<PaneOp> panes;
    std::vector<Graft> grafts = {};      ///< UI parts copied in from other layouts
    std::vector<AspectFix> aspect = {};  ///< top-screen layouts to keep in proportion
    std::vector<AnimWrapParams> anim_wraps = {}; ///< applied after the grafts
    std::vector<BottomAlign> bottom_align = {};
    /// Top-screen layouts rescaled around a pivot (not aspect related), e.g. to make room for
    /// bottom-screen content placed below them.
    std::vector<RescaleFix> rescale = {};
};

// The remaster UI patch table. Backgrounds become key-coloured (rendered transparent by the
// Leaf compositor), and elements that are moved to the main screen lose decorations that only
// made sense on the bottom screen.
const std::vector<ArcPatch>& Patches() {
    static const std::vector<ArcPatch> patches = {
        // Scrolling bottom-screen wallpaper shown behind most menus and dialogs. Hidden: the
        // render target is cleared to the key colour instead. (Its animations tint vertex
        // colours, so recolouring it is not enough.)
        {"Layout/Menu/mvmnt_undscrn.arc",
         true,
         {
             {"blyt/mvmnt_undscrn.bclyt", "P_base_00", PaneOpKind::Hide},
             {"blyt/mvmnt_undscrn.bclyt", "P_base_01", PaneOpKind::Hide},
         }},
        // Title screen wallpaper. All layers are hidden except one, which is moved off-screen:
        // it is still drawn, so the UI probe can tell the title screen is up.
        {"Layout/Title/opng_titl.arc",
         false,
         {
             {"blyt/opng_titl_bg.bclyt", "P_bg_01", PaneOpKind::Hide},
             {"blyt/opng_titl_bg.bclyt", "P_bg_02", PaneOpKind::Hide},
             {"blyt/opng_titl_bg.bclyt", "P_bg_03", PaneOpKind::Hide},
             {"blyt/opng_titl_bg.bclyt", "P_bg_04", PaneOpKind::Hide},
             {"blyt/opng_titl_bg.bclyt", "P_filter", PaneOpKind::Hide},
             {"blyt/opng_titl_bg.bclyt", "P_bg_00", PaneOpKind::Translate, 4000.0f, 4000.0f},
             // Only "Press A" stays on the title screen.
             {"blyt/opng_titl_00.bclyt", "T_cNin_00", PaneOpKind::Hide},
             {"blyt/opng_titl_00.bclyt", "T_home_00", PaneOpKind::Hide},
         },
         {
             // ...sitting in the same cream bubble the title menu uses. The window is built at
             // its natural 64px height (its corner pieces are 32px) and scaled down.
             {"Layout/selWin/cmn_slct_win.arc",
              {"blyt/opng_titl_00.bclyt", "blyt/cmn_slct_win.bclyt", "W_slct_00",
               "W_leaf_press", "T_tex_00", 0.0f, 23.0f, 180.0f, 64.0f, 0.8f, 0.72f}},
         },
         {{"blyt/opng_titl_logo.bclyt", 0.0f, 0.0f}, {"blyt/opng_titl_logo_s.bclyt", 0.0f, 0.0f}},
         // The bubble pops in and pulses together with "Press A".
         {{"blyt/opng_titl_00.bclyt",
           "W_leaf_press",
           "T_tex_00",
           "T_tex_00",
           "N_leaf_press",
           {"anim/opng_titl_00_in.bclan", "anim/opng_titl_00_loop.bclan"}}}},
        // Top-screen 2D art that is not otherwise changed: only kept in proportion.
        {"Layout/msgWin/msg_win.arc", false, {}, {},
         {{"blyt/msg_win.bclyt", 0.0f, -120.0f}, {"blyt/msg_only.bclyt", 0.0f, -120.0f}}},
        // Date/time plate: lined up with the minimap in the bottom-right corner.
        {"Layout/Menu/time_bel_win.arc", false, {}, {},
         {{"blyt/time_bel_win.bclyt", 0.0f, 120.0f}, {"blyt/event_timer.bclyt", -200.0f, 120.0f}},
         {},
         {{"blyt/time_bel_win.bclyt", "N_time", -117.5f, 120.0f, 4.0f}}},
        {"Layout/Telop/telop.arc", false, {}, {}, {{"blyt/telop.bclyt", 0.0f, 0.0f}}},
        // Iris wipe: kept round. Its black side panels only reach the screen edges at 5:3, so
        // they are made larger (their animated sides are unaffected) to cover any aspect.
        {"Layout/Wipe/wipe_circle.arc",
         false,
         {
             {"blyt/wipe_circle.bclyt", "P_T", PaneOpKind::Size, 1200.0f, 240.0f},
             {"blyt/wipe_circle.bclyt", "P_B", PaneOpKind::Size, 1200.0f, 240.0f},
             {"blyt/wipe_circle.bclyt", "P_L", PaneOpKind::Size, 240.0f, 1200.0f},
             {"blyt/wipe_circle.bclyt", "P_R", PaneOpKind::Size, 240.0f, 1200.0f},
         },
         {},
         {{"blyt/wipe_circle.bclyt", 0.0f, 0.0f}}},
        {"Layout/common/caution.arc", false, {}, {}, {{"blyt/caution_T.bclyt", 0.0f, 0.0f}}},
        // The saving spinner (save_wait.bclyt) is not fixed itself: the game places it with
        // the transform of a pane in the message window, which already carries that window's
        // fix (fixing both squeezed it twice).
        {"Layout/common/save_wait.arc", false, {}, {},
         {{"blyt/load_wait.bclyt", 0.0f, 0.0f}, {"blyt/load_wait_b.bclyt", 0.0f, 0.0f}}},
        {"Layout/Board/board.arc", false, {}, {},
         {{"blyt/board_base_T.bclyt", 0.0f, 0.0f}, {"blyt/board_paper.bclyt", 0.0f, 0.0f}}},
        {"Layout/bookWin/m_book.arc", false, {}, {}, {{"blyt/m_book_T.bclyt", 0.0f, 0.0f}}},
        {"Layout/catalog/catalogue.arc", false, {}, {}, {{"blyt/ctlg_T.bclyt", 0.0f, 0.0f}}},
        {"Layout/passWin/m_psprt.arc", false, {}, {}, {{"blyt/bdg_T.bclyt", 0.0f, 0.0f}}},
        {"Layout/TourList/tour_list.arc", false, {}, {}, {{"blyt/tour_list_T.bclyt", 0.0f, 0.0f}}},
        {"Layout/event/event.arc", false, {}, {},
         {{"blyt/event_list.bclyt", 0.0f, 0.0f}, {"blyt/event_sign.bclyt", 0.0f, 0.0f}}},
        {"Layout/credit/credit.arc", false, {}, {},
         {{"blyt/crdt_credit.bclyt", 0.0f, 0.0f},
          {"blyt/crdt_history.bclyt", 0.0f, 0.0f},
          {"blyt/crdt_licence.bclyt", 0.0f, 0.0f},
          {"blyt/crdt_name.bclyt", 0.0f, 0.0f},
          {"blyt/crdt_time.bclyt", 0.0f, 0.0f}}},
        // Choice bubble: drop the tail that pointed up at the top screen, and make the
        // translucent body opaque so it does not mix with the key colour behind it.
        // Menu tabs: the orange close tab (top right) is dropped, B closes the menus. On the
        // main screen it covered values such as Welcome amiibo's coupon count. Its picture and
        // touch area are hidden (the game switches the tab itself on and off).
        {"Layout/Menu/menu_tab.arc",
         false,
         {
             {"blyt/menu_tab.bclyt", "im_tab_05", PaneOpKind::Hide},
             {"blyt/menu_tab.bclyt", "B_back_00", PaneOpKind::Hide},
             // Welcome amiibo's Miiverse "post a screenshot" button: the service is gone and
             // the game waits forever for it, so the button is removed.
             {"blyt/menu_tab.bclyt", "P_post_00", PaneOpKind::Hide},
             {"blyt/menu_tab.bclyt", "B_post_00", PaneOpKind::Hide},
         }},
        // Welcome amiibo passport, cat tasks page: the task details window (top screen) shrinks
        // a little and moves down, out from under the coupon count in the top-right corner (so
        // Leaf can show it alone over the task list) and behind the passport's page tabs (so
        // it does not peek out above the list). Scaling by 0.847 around (0, -147.8) puts its
        // top edge at y 52, its bottom edge at the screen's and keeps it centred.
        {"Layout/passWin/m_plife.arc",
         false,
         {},
         {},
         {},
         {},
         {},
         {{"blyt/plifeT_00.bclyt", 0.847f, 0.0f, -147.8f}}},
        // Welcome amiibo coupon machine: the bottom screen's scenery is dropped, so its menu
        // fills the main screen with the top screen (sky) visible around the buttons; the big
        // coupon picture on the top screen is dropped too.
        {"Layout/ticket/ticket.arc",
         false,
         {
             {"blyt/tct_BG_00.bclyt", "P_sky_00", PaneOpKind::Hide},
             {"blyt/tct_BG_00.bclyt", "P_grand_00", PaneOpKind::Hide},
             {"blyt/tct_BG_00.bclyt", "P_cloud_00", PaneOpKind::Hide},
             {"blyt/tct_BG_00.bclyt", "P_cloud_02", PaneOpKind::Hide},
             // The scrolling mountains and trees are shrunk to nothing, not hidden: the game only
             // redraws the bottom screen while its animation runs, and its messages
             // ("no MEOW Coupons to collect") would otherwise be drawn once and lost.
             {"blyt/tct_BG_00.bclyt", "P_mt_00", PaneOpKind::Size, 0.0f, 0.0f},
             {"blyt/tct_BG_00.bclyt", "P_mt_01", PaneOpKind::Size, 0.0f, 0.0f},
             {"blyt/tct_BG_00.bclyt", "P_mt_02", PaneOpKind::Size, 0.0f, 0.0f},
             {"blyt/tct_BG_00.bclyt", "P_mt_03", PaneOpKind::Size, 0.0f, 0.0f},
             {"blyt/tct_BG_00.bclyt", "P_mt_04", PaneOpKind::Size, 0.0f, 0.0f},
             {"blyt/tct_BG_00.bclyt", "P_mt_05", PaneOpKind::Size, 0.0f, 0.0f},
             {"blyt/tct_BG_00.bclyt", "P_mt_06", PaneOpKind::Size, 0.0f, 0.0f},
             {"blyt/tct_BG_00.bclyt", "P_mt_07", PaneOpKind::Size, 0.0f, 0.0f},
             {"blyt/tct_BG_00.bclyt", "P_mt_08", PaneOpKind::Size, 0.0f, 0.0f},
             {"blyt/tct_BG_00.bclyt", "P_mt_09", PaneOpKind::Size, 0.0f, 0.0f},
             {"blyt/tct_BG_00.bclyt", "P_tree_00", PaneOpKind::Size, 0.0f, 0.0f},
             {"blyt/tct_BG_00.bclyt", "P_tree_01", PaneOpKind::Size, 0.0f, 0.0f},
             {"blyt/tct_BG_00.bclyt", "P_tree_02", PaneOpKind::Size, 0.0f, 0.0f},
             {"blyt/tct_BG_00.bclyt", "P_tree_03", PaneOpKind::Size, 0.0f, 0.0f},
             {"blyt/tct_BG_00.bclyt", "P_tree_04", PaneOpKind::Size, 0.0f, 0.0f},
             {"blyt/tct_BG_00.bclyt", "P_tree_05", PaneOpKind::Size, 0.0f, 0.0f},
             {"blyt/tct_BG_00.bclyt", "P_tree_06", PaneOpKind::Size, 0.0f, 0.0f},
             {"blyt/tct_BG_00.bclyt", "P_tree_07", PaneOpKind::Size, 0.0f, 0.0f},
             {"blyt/tct_BG_00.bclyt", "P_tree_08", PaneOpKind::Size, 0.0f, 0.0f},
             {"blyt/tct_BG_00.bclyt", "P_tree_09", PaneOpKind::Size, 0.0f, 0.0f},
             {"blyt/tct_BG_00.bclyt", "P_tree_10", PaneOpKind::Size, 0.0f, 0.0f},
             {"blyt/tct_BG_00.bclyt", "P_tree_11", PaneOpKind::Size, 0.0f, 0.0f},
             {"blyt/tct_BG_00.bclyt", "P_tree_12", PaneOpKind::Size, 0.0f, 0.0f},
             {"blyt/tct_BG_00.bclyt", "P_tree_13", PaneOpKind::Size, 0.0f, 0.0f},
             {"blyt/tct_BG_00.bclyt", "P_tree_14", PaneOpKind::Size, 0.0f, 0.0f},
             {"blyt/tct_BG_00.bclyt", "P_tree_15", PaneOpKind::Size, 0.0f, 0.0f},
             {"blyt/tct_menuT_00.bclyt", "N_ticket_00", PaneOpKind::Hide},
             // Sky (top screen): kept in proportion like the other art; the sky panel itself
             // is enlarged so it still fills the screen after the aspect scaling.
             {"blyt/tct_BGT_00.bclyt", "P_sky_00", PaneOpKind::Size, 1200.0f, 600.0f},
         },
         {},
         {{"blyt/tct_BGT_00.bclyt", 0.0f, -120.0f}}},
        {"Layout/selWin/slct_win.arc",
         false,
         {
             {"blyt/slct_win.bclyt", "P_kuchi_00", PaneOpKind::Hide},
             {"blyt/slct_win.bclyt", "P_kuchi_sh_00", PaneOpKind::Hide},
             {"blyt/slct_win.bclyt", "P_pattern_00", PaneOpKind::Alpha, 255.0f},
         }},
    };
    return patches;
}

u64 ModId(u64 program_id) {
    constexpr u64 UpdateMask = 0x0000000e'00000000;
    if ((program_id & 0x000000ff'00000000) == UpdateMask)
        return program_id & ~UpdateMask;
    return program_id;
}

std::string ModsRoot(u64 program_id) {
    return fmt::format("{}mods/{:016X}/", FileUtil::GetUserPath(FileUtil::UserPath::LoadDir),
                       ModId(program_id));
}

/// Scale that keeps top-screen 2D art in proportion when the 400x240 image is shown at a
/// different aspect ratio: shrink along the stretched axis so everything still fits.
std::pair<float, float> AspectScale() {
    return ArtScale();
}

std::string MarkerContents(u64 program_id) {
    const auto [sx, sy] = AspectScale();
    return fmt::format("leaf-ui {} {:016X} {:.4f} {:.4f}\n", PatchVersion, program_id, sx, sy);
}

bool ApplyPatch(const ArcPatch& patch, std::vector<u8>& arc) {
    bool changed = false;
    for (const auto& file : ListArc(arc)) {
        u8* data = arc.data() + file.offset;
        if (file.offset + file.size > arc.size())
            continue;
        if (patch.key_all_textures && file.path.ends_with(".bclim"))
            changed |= FillBclimWithKey(data, file.size);
        for (const auto& op : patch.panes) {
            if (file.path != op.layout)
                continue;
            const u32 pane = FindPane(data, file.size, op.pane);
            if (pane == 0) {
                LOG_WARNING(Core, "Leaf: pane {} not found in {}", op.pane, patch.path);
                continue;
            }
            switch (op.kind) {
            case PaneOpKind::Hide:
                SetPaneVisible(data, pane, false);
                break;
            case PaneOpKind::Alpha:
                SetPaneAlpha(data, pane, static_cast<u8>(op.x));
                break;
            case PaneOpKind::Translate:
                SetPaneTranslate(data, pane, op.x, op.y);
                break;
            case PaneOpKind::Scale:
                SetPaneScale(data, pane, op.x, op.y);
                break;
            case PaneOpKind::Size:
                SetPaneSize(data, pane, op.x, op.y);
                break;
            case PaneOpKind::WhiteVertexColors:
                SetPictureVertexColors(data, pane, 0xFFFFFFFF);
                break;
            }
            changed = true;
        }
    }
    return changed;
}

} // namespace

bool PrepareMods(const std::string& rom_path, std::string& error) {
    auto loader = Loader::GetLoader(rom_path);
    if (!loader) {
        error = "Unable to open the ROM";
        return false;
    }
    u64 program_id = 0;
    loader->ReadProgramId(program_id);
    if (!IsLeafTitle(program_id)) {
        error = "This ROM is not Animal Crossing: New Leaf";
        return false;
    }

    const std::string root = ModsRoot(program_id);
    const std::string marker_path = root + "leaf_ui_version.txt";
    std::string marker;
    FileUtil::ReadFileToString(true, marker_path, marker);
    if (marker == MarkerContents(program_id))
        return true;

    // Remove our previous output first so the RomFS below yields the original files: what the
    // last run wrote (listed next to the marker) and what this run patches.
    const std::string list_path = root + "leaf_ui_files.txt";
    std::string previous;
    FileUtil::ReadFileToString(true, list_path, previous);
    std::istringstream previous_files(previous);
    for (std::string path; std::getline(previous_files, path);)
        if (!path.empty() && path.find("..") == std::string::npos)
            FileUtil::Delete(root + "romfs/" + path);
    for (const auto& patch : Patches())
        FileUtil::Delete(root + "romfs/" + patch.path);
    FileUtil::Delete(list_path);
    loader = Loader::GetLoader(rom_path);

    std::shared_ptr<FileSys::RomFSReader> romfs;
    if (!loader || loader->ReadRomFS(romfs) != Loader::ResultStatus::Success || !romfs) {
        error = "Unable to read the ROM's RomFS (is it decrypted?)";
        return false;
    }
    const auto files = ListRomFS(*romfs);
    const auto [scale_x, scale_y] = AspectScale();
    std::string written; ///< archives written, one RomFS path per line

    for (const auto& patch : Patches()) {
        const auto it = files.find(patch.path);
        if (it == files.end()) {
            LOG_WARNING(Core, "Leaf: {} not present in this ROM", patch.path);
            continue;
        }
        auto arc = ReadRomFSFile(*romfs, it->second);
        bool changed = ApplyPatch(patch, arc);
        const bool aspect = !patch.aspect.empty() && (scale_x != 1.0f || scale_y != 1.0f);
        if (!patch.grafts.empty() || !patch.anim_wraps.empty() ||
            !patch.bottom_align.empty() || !patch.rescale.empty() || aspect) {
            auto entries = ReadArcEntries(arc);
            for (const auto& graft : patch.grafts) {
                const auto source = files.find(graft.source_arc);
                if (source == files.end() ||
                    !GraftWindow(entries, ReadArcEntries(ReadRomFSFile(*romfs, source->second)),
                                 graft.params)) {
                    LOG_WARNING(Core, "Leaf: could not graft {} into {}", graft.params.new_name,
                                patch.path);
                    continue;
                }
                changed = true;
            }
            for (const auto& wrap : patch.anim_wraps) {
                if (WrapForAnimation(entries, wrap))
                    changed = true;
                else
                    LOG_WARNING(Core, "Leaf: could not wrap {} in {}", wrap.wrapper, patch.path);
            }
            for (const auto& fix : patch.rescale) {
                if (WrapRootChildren(entries, fix.layout, fix.scale, fix.scale, fix.pivot_x,
                                     fix.pivot_y))
                    changed = true;
                else
                    LOG_WARNING(Core, "Leaf: could not rescale {} in {}", fix.layout, patch.path);
            }
            for (const auto& align : patch.bottom_align) {
                const float target = -120.0f + align.margin;
                const float dy = (target - align.pivot_y) / scale_y + align.pivot_y -
                                 align.visible_bottom;
                if (ShiftChildren(entries, align.layout, align.parent, 0.0f, dy))
                    changed = true;
                else
                    LOG_WARNING(Core, "Leaf: could not move {} in {}", align.parent, patch.path);
            }
            if (aspect) {
                for (const auto& fix : patch.aspect) {
                    if (WrapRootChildren(entries, fix.layout, scale_x, scale_y, fix.pivot_x,
                                         fix.pivot_y))
                        changed = true;
                    else
                        LOG_WARNING(Core, "Leaf: no aspect fix for {} in {}", fix.layout,
                                    patch.path);
                }
            }
            arc = WriteArc(entries);
        }
        if (!changed)
            continue;
        const std::string out = root + "romfs/" + patch.path;
        FileUtil::CreateFullPath(out);
        FileUtil::IOFile file(out, "wb");
        if (!file.IsOpen() || file.WriteBytes(arc.data(), arc.size()) != arc.size()) {
            error = "Unable to write " + out;
            return false;
        }
        written += patch.path;
        written += '\n';
    }
    FileUtil::WriteStringToFile(true, list_path, written);
    FileUtil::WriteStringToFile(true, marker_path, MarkerContents(program_id));
    LOG_INFO(Core, "Leaf: UI mods generated in {}", root);
    return true;
}

void EnsureMods(const std::string& rom_path) {
    auto loader = Loader::GetLoader(rom_path);
    u64 program_id = 0;
    if (!loader || loader->ReadProgramId(program_id) != Loader::ResultStatus::Success ||
        !IsLeafTitle(program_id))
        return;
    std::string error;
    if (!PrepareMods(rom_path, error))
        LOG_WARNING(Core, "Leaf: UI mods not generated: {}", error);
}

std::unordered_map<std::string, u64> BuildTextureKeys(Loader::AppLoader& loader) {
    std::unordered_map<std::string, u64> keys;
    std::shared_ptr<FileSys::RomFSReader> romfs;
    if (loader.ReadRomFS(romfs) != Loader::ResultStatus::Success || !romfs)
        return keys;
    for (const auto& [path, entry] : ListRomFS(*romfs)) {
        if (!path.starts_with("Layout/") || !path.ends_with(".arc"))
            continue;
        const auto arc = ReadRomFSFile(*romfs, entry);
        const std::string arc_name = path.substr(7);
        for (const auto& file : ListArc(arc)) {
            if (!file.path.ends_with(".bclim") || file.offset + file.size > arc.size())
                continue;
            const u8* data = arc.data() + file.offset;
            const auto info = ReadBclimInfo(data, file.size);
            if (!info)
                continue;
            const auto slash = file.path.rfind('/');
            const std::string texture =
                slash == std::string::npos ? file.path : file.path.substr(slash + 1);
            keys[arc_name + ":" + texture] =
                UiProbe::TextureKey(data, info->data_size, info->width, info->height);
        }
    }
    return keys;
}

} // namespace Leaf::UiPatcher
