// Copyright 2026 Poly
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Generates the "remaster" UI layout mods from the user's own ROM and keeps the table of UI
// texture hashes used by the bottom-screen scene detection.

#pragma once

#include <string>
#include <unordered_map>
#include "common/common_types.h"

namespace Loader {
class AppLoader;
}

namespace Leaf::UiPatcher {

/// Bumped whenever the patch table changes so existing installs regenerate their mods.
constexpr int PatchVersion = 8;

/// Writes patched layout archives to <user>/load/mods/<title>/romfs. Safe to call on every
/// launch: does nothing when the mods for this ROM and PatchVersion already exist.
/// Returns false and fills `error` on failure.
bool PrepareMods(const std::string& rom_path, std::string& error);

/// Generates the mods for a Leaf ROM before it boots. Never fails the boot: problems are
/// logged and the game runs with its original UI. Other titles are ignored.
void EnsureMods(const std::string& rom_path);

/// Hashes every UI texture of the running game (read through LayeredFS, so patched textures
/// hash as they will appear in memory). Keys are "Dir/file.arc:texture.bclim".
std::unordered_map<std::string, u64> BuildTextureKeys(Loader::AppLoader& loader);

} // namespace Leaf::UiPatcher
