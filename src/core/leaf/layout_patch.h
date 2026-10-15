// Copyright 2026 Poly
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Helpers for reading the game's RomFS and patching its UI layout archives in place:
//   RomFS (level 3) -> .arc (darc) -> .bclyt (CTR layout) / .bclim (CTR texture)

#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/common_types.h"

namespace FileSys {
class RomFSReader;
}

namespace Leaf::LayoutPatch {

struct FileEntry {
    u64 offset; ///< within the reader
    u64 size;
};

/// Lists every file in a RomFS level-3 image, keyed by path without a leading slash.
std::unordered_map<std::string, FileEntry> ListRomFS(FileSys::RomFSReader& reader);

std::vector<u8> ReadRomFSFile(FileSys::RomFSReader& reader, const FileEntry& entry);

struct ArcFile {
    std::string path; ///< e.g. "blyt/slct_win.bclyt" (no leading "./")
    u32 offset;
    u32 size;
};

/// Lists the files of a darc archive. Returns an empty list if the data is not a darc.
std::vector<ArcFile> ListArc(const std::vector<u8>& arc);

struct TextureInfo {
    u32 width;
    u32 height;
    u32 format; ///< BCLIM format id (10 = ETC1, 11 = ETC1A4, ...)
    u32 data_size;
};

std::optional<TextureInfo> ReadBclimInfo(const u8* data, u32 size);

/// Fills a BCLIM's pixels with the key colour (magenta). Supports ETC1 and ETC1A4.
bool FillBclimWithKey(u8* data, u32 size);

/// Offset of a pane section (pan1/pic1/txt1/wnd1/bnd1/prt1) by name inside a CLYT, or 0.
u32 FindPane(const u8* clyt, u32 size, const std::string& name);

void SetPaneVisible(u8* clyt, u32 pane, bool visible);
void SetPaneAlpha(u8* clyt, u32 pane, u8 alpha);
void SetPaneTranslate(u8* clyt, u32 pane, float x, float y);
void SetPaneScale(u8* clyt, u32 pane, float x, float y);
void SetPaneSize(u8* clyt, u32 pane, float width, float height);

/// Sets the four corner vertex colours of a picture pane (pic1), e.g. to remove gradients.
void SetPictureVertexColors(u8* clyt, u32 pane, u32 rgba);

// --- Structural edits (archive / layout rebuilding) -----------------------------------------

struct ArcEntry {
    std::string path; ///< e.g. "timg/my_csw_LT.bclim"
    std::vector<u8> data;
};

/// Reads every file of a darc archive.
std::vector<ArcEntry> ReadArcEntries(const std::vector<u8>& arc);

/// Writes a darc archive with the same conventions as the game's (uppercase-sorted entries,
/// textures aligned to 0x80, other files to 0x20).
std::vector<u8> WriteArc(const std::vector<ArcEntry>& entries);

struct GraftWindowParams {
    std::string dst_layout; ///< layout inside the destination archive
    std::string src_layout; ///< layout inside the source archive
    std::string src_pane;   ///< window pane (wnd1) to copy
    std::string new_name;   ///< name of the new pane (max 16 chars)
    std::string before;     ///< insert before this pane (drawn behind it)
    float x, y, width, height, scale_x, scale_y;
};

/// Copies a window pane, its materials and their textures from one layout archive into
/// another, so the game draws one of its own UI parts somewhere it originally was not.
bool GraftWindow(std::vector<ArcEntry>& dst, const std::vector<ArcEntry>& src,
                 const GraftWindowParams& params);

/// Inserts two wrapper panes between RootPane and its children that scale everything by
/// (scale_x, scale_y) around (pivot_x, pivot_y). Used to undo the horizontal stretch of the
/// top screen's 2D art when the game is shown at 16:9 or 4:3. Animations keep working because
/// they address the original panes by name.
bool WrapRootChildren(std::vector<ArcEntry>& arc, const std::string& layout, float scale_x,
                      float scale_y, float pivot_x, float pivot_y);

struct AnimWrapParams {
    std::string layout;            ///< layout inside the archive
    std::string first, last;       ///< consecutive sibling panes to wrap
    std::string animated;          ///< one of them, whose animations move to the wrapper
    std::string wrapper;           ///< name of the new null pane (max 16 chars)
    std::vector<std::string> anims; ///< .bclan files animating `animated`
};
/// Wraps sibling panes in a new null pane that takes over the transform and the animations
/// of one of them, so a part grafted next to an animated pane appears and moves with it.
bool WrapForAnimation(std::vector<ArcEntry>& arc, const AnimWrapParams& params);

/// Moves the direct children of a pane (not the pane itself, which may be animated).
bool ShiftChildren(std::vector<ArcEntry>& arc, const std::string& layout,
                   const std::string& parent, float dx, float dy);

/// Hides panes by name in a layout stored in an archive. Returns the number of panes hidden.
int HidePanes(std::vector<ArcEntry>& arc, const std::string& layout,
              const std::vector<std::string>& panes);

} // namespace Leaf::LayoutPatch
