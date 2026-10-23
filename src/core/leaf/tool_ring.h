// Copyright 2026 Poly
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Tool ring for Animal Crossing: New Leaf (original and Welcome amiibo), like New Horizons':
// the tools in the pockets around the player, one to pick, the one in hand to put away.
//
// Switching goes through the game's own "equip" routine, the one the pockets screen calls when
// an item is dropped onto the player's picture, and the pockets are changed the way that screen
// changes them (the tool taken out leaves its slot to the one put away). Saves only ever contain
// what the unmodified game writes.
//
// The game's code is found by signature at load time. The player's per-frame update is
// redirected to a stub in the zero padding at the end of the code segment; the stub asks the
// emulator (through a Leaf SVC) whether a switch is pending, calls the equip routine if so and
// continues into the update.

#pragma once

#include <vector>
#include "common/common_types.h"

namespace Core {
class System;
}
namespace Kernel {
class Process;
}

namespace Leaf::ToolRing {

/// SVC number used by the stub (unused by the 3DS kernel, Azahar and the auto-stack stubs).
constexpr u32 RingSvc = 0xAD;

/// Tool kinds (the game's item categories, the same in both releases).
enum Kind : u8 {
    Axe = 0x0E,
    Net = 0x0F,
    Rod = 0x10,
    Shovel = 0x11,
    Slingshot = 0x12,
    WateringCan = 0x13,
    Megaphone = 0x15,
};

struct Tool {
    int slot;    ///< pocket slot, -1 = in the player's hand
    u8 kind;     ///< Kind
    u8 variant;  ///< 0 rental, 1 regular, 2 silver, 3 gold (always 0 for the megaphone)
    u16 icon;    ///< ItemWin/item_icon.arc icn_<icon>.bclim
};

/// Most tools the ring shows.
constexpr std::size_t MaxTools = 8;

/// Forgets the located code (another program is being loaded).
void Reset();

/// Finds the game code the ring needs. Called when the program is loaded, before it runs.
void Locate(Core::System& system, Kernel::Process& process);

/// Installs the stub and the redirect. Safe to call at any time while the game runs (after boot,
/// after loading a savestate).
void Apply(Core::System& system);

/// True when every part was found in the loaded game.
bool Available();

/// The tools to show, or nothing when the ring can't be used right now (the player isn't
/// standing still on their own, or there are no tools). The tool in hand comes first; then one
/// of each different tool in the pockets, then further tools in pocket order.
std::vector<Tool> Tools();

/// Takes out the tool in pocket `slot`, or puts away the one in hand for slot -1. Done on the
/// game's next player update if the player is still standing.
void Select(int slot);

/// Handles the ring SVC; returns false for any other SVC (or one not issued by the ring stub).
bool HandleSvc(Core::System& system, u32 immediate);

} // namespace Leaf::ToolRing
