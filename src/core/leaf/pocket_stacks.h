// Copyright 2026 Poly
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Optional "auto-stack on pickup" for Animal Crossing: New Leaf (original and Welcome amiibo).
//
// The game already stacks some items in the pockets when the player drags one onto another:
// fruit (baskets of up to 9), turnips (up to 100), bells bags (up to 99,000) and stationery of
// the same design (up to 4). It never does this on pickup. When enabled, a picked-up item goes
// into the first stack of the same kind with enough room, and a full pocket no longer refuses an
// item that fits into a stack. Stacks are written exactly as the game's own drag-and-drop merge
// writes them, so saves only ever contain data the unmodified game produces.
//
// The game's code is found by signature at load time. Two calls are redirected to small stubs in
// the zero padding at the end of the code segment; each stub asks the emulator (through a Leaf
// SVC) whether to handle the call and otherwise continues into the original function.

#pragma once

#include "common/common_types.h"

namespace Core {
class System;
}
namespace Kernel {
class Process;
}

namespace Leaf::PocketStacks {

/// SVC number used by the stubs (unused by the 3DS kernel and by Azahar's custom SVCs).
constexpr u32 LeafSvc = 0xAC;

/// Forgets the located code (another program is being loaded).
void Reset();

/// Finds the game code the patch needs. Called when the program is loaded, before it runs.
void Locate(Core::System& system, Kernel::Process& process);

/// Writes or removes the patch according to Config::auto_stack. Safe to call at any time while
/// the game runs (after boot, after loading a savestate).
void Apply(Core::System& system);

/// True when every part of the patch was found in the loaded game.
bool Available();

/// Handles the Leaf SVC; returns false for any other SVC (or one not issued by a Leaf stub).
bool HandleSvc(Core::System& system, u32 immediate);

} // namespace Leaf::PocketStacks
