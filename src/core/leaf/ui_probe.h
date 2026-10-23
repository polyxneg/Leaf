// Copyright 2026 Poly
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Detects which of the game's UI layouts are on the 3DS bottom screen by watching which textures
// are drawn into the bottom screen's render target. Layout textures (BCLIM) are uploaded in the
// GPU's native format, so their bytes in memory equal the bytes in the RomFS file and can be
// identified by hash without understanding the game's internal state.

#pragma once

#include <array>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include "common/common_types.h"
#include "common/math_util.h"

namespace Memory {
class MemorySystem;
}

namespace Leaf::UiProbe {

/// Called for every PICA draw with the colour buffer and the first texture unit's state.
/// `bounds` is the draw's rectangle in render target pixels (min x, min y, max x, max y) when it
/// was measured (see WantBounds), else null.
void OnDraw(PAddr color_buffer, bool texture_enabled, PAddr texture, u32 width, u32 height,
            u32 format, const std::array<float, 4>* bounds = nullptr);

/// Whether draws should be measured (costs a CPU vertex-shader pass per small UI draw, so only
/// while a scene that places elements by their measured bounds is up).
bool WantBounds();
void SetWantBounds(bool want);

/// Called for every display transfer; transfers to a bottom-screen sized output identify the
/// render target the game draws its bottom screen into.
void OnDisplayTransfer(PAddr input, PAddr output, u32 input_size, u32 output_width,
                       u32 output_height);

/// While any of these textures is on the top screen, the bottom screen is one the game draws
/// once and then keeps sending out without drawing (expecting the render target to still hold
/// it): the last drawn bottom screen stays up instead of the undrawn ones.
void SetHoldTopKeys(std::unordered_set<u64> keys);
bool HoldBottom();

/// Called when the game swaps the bottom-screen framebuffer it displays.
void OnBottomSwap(PAddr shown);

/// The framebuffer to display for `shown`: the last drawn bottom screen while the game shows
/// one it sent without drawing in between redraws. Other addresses are returned unchanged.
PAddr DisplayedBottom(PAddr shown);

/// Resolves the textures drawn this frame. Called once per emulated frame on the emu thread.
void EndFrame(Memory::MemorySystem& memory);

/// Hash used to identify a texture: XXH3-64 of its pixel data mixed with its dimensions.
u64 TextureKey(const u8* data, std::size_t size, u32 width, u32 height);

/// Bits per pixel of a PICA texture format (0 if unknown).
u32 BitsPerPixel(u32 format);

/// Registers a known UI texture (computed from the user's ROM) under a stable numeric id.
void RegisterTexture(u64 key, u32 id);
void ClearRegisteredTextures();

/// Ids of the known textures drawn on the bottom screen in the last frame that drew anything.
std::vector<u32> VisibleTextures();

/// Keys of every texture seen on the bottom screen last frame (for tooling / discovery).
std::vector<u64> LastFrameKeys();

/// Keys of every texture drawn to the top screen in the last frame.
std::vector<u64> LastFrameTopKeys();

/// Diagnostics: the last few display transfers (address, screen, textures and draws before).
std::string RecentTransfers();

/// Measured rectangles of the textures drawn to the bottom screen in the last frame, in
/// bottom-screen pixels (320x240, y down). Only filled while WantBounds() is set.
std::vector<std::pair<u64, Common::Rectangle<float>>> LastFrameBounds();

} // namespace Leaf::UiProbe
