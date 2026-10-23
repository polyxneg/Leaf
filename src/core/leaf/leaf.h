// Copyright 2026 Poly
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Game-specific support for Animal Crossing: New Leaf ("Leaf").
// Patches are located by signature at load time, so any region/revision of the user's own ROM
// can be used; nothing from the game is shipped with the app.

#pragma once

#include <atomic>
#include <utility>
#include "common/common_types.h"
#include "common/math_util.h"

namespace Core {
class System;
}
namespace Kernel {
class Process;
}

namespace Leaf {

/// Native aspect ratio of the 3DS top screen (400x240).
constexpr float NativeAspect = 400.0f / 240.0f;

/// How the main (top) screen is presented when the Leaf single-screen layout is active.
enum class DisplayMode : int {
    Wide16x9 = 0,   ///< 16:9, letterboxed/pillarboxed to fit the display
    Classic4x3 = 1, ///< 4:3, fitted to the display
    Native5x3 = 2,  ///< original 400x240 proportions
    Fill = 3,       ///< match the display's own aspect ratio exactly
};

enum class PanelAnchor : int { Center = 0, Right = 1, Left = 2 };

struct Config {
    /// Aspect ratio the game camera should render with. 0 keeps the native 5:3 projection.
    /// Read by the game once at boot, so it must be set before the ROM is loaded.
    std::atomic<float> camera_aspect{0.0f};

    /// Single-screen layout: the main screen fills the display and bottom-screen UI elements
    /// are placed onto it. Always on while ACNL runs; there is no two-screen mode.
    std::atomic<bool> layout_enabled{false};
    std::atomic<int> display_mode{static_cast<int>(DisplayMode::Wide16x9)};
    std::atomic<bool> panel_visible{false};
    std::atomic<float> panel_scale{0.82f}; ///< panel height as a fraction of the display height
    std::atomic<float> panel_opacity{0.96f};
    std::atomic<int> panel_anchor{static_cast<int>(PanelAnchor::Center)};

    /// Auto-stack on pickup (see pocket_stacks.h). Off by default; off = the original game.
    std::atomic<bool> auto_stack{false};

    /// The tool ring is open (see tool_ring.h): the HUD (Leaf's elements and the game's clock
    /// plate) is left out meanwhile.
    std::atomic<bool> tool_ring_open{false};

    /// Refresh rate of the device display (Hz), 0 if unknown. Used to pace emulation so every
    /// display refresh gets a new frame (see PaceScale).
    std::atomic<float> display_refresh{0.0f};
};

/// Height/width ratio of the main screen for a display mode on a window of the given size.
float DisplayAspect(DisplayMode mode, u32 window_width, u32 window_height);

/// Camera aspect (width/height) that matches a display mode on a window of the given size.
float CameraAspectFor(DisplayMode mode, u32 window_width, u32 window_height);

Config& GetConfig();

/// True while an Animal Crossing: New Leaf process is loaded.
bool IsActive();

/// Returns true for any regional release of Animal Crossing: New Leaf.
bool IsLeafTitle(u64 title_id);

/// True while the Welcome amiibo release (1.5, own title ID) is running.
bool IsWelcomeAmiibo();

/// Called by Core::System after the application process has been created but before it runs.
void OnProgramLoaded(Core::System& system, u64 title_id, Kernel::Process& process);

/// Called after a savestate was loaded: re-applies code patches that live in game memory.
void OnStateLoaded(Core::System& system);

/// Re-applies the camera aspect to a running game (takes effect on the next camera update).
void ApplyCameraAspect(Core::System& system);

/// True once the game's camera parameters were located in the loaded executable.
bool CameraPatchAvailable();

/// Part of the top screen to present (normalized), or nullptr for all of it. The game never draws
/// the first row of the top screen; on hardware it keeps leftover bottom-screen pixels, with the
/// Leaf layout it would show the transparency key.
const Common::Rectangle<float>* TopScreenCrop();

/// Speed factor for the frame limiter. The 3DS refreshes at 59.83 Hz; on a 60 Hz (or 120 Hz)
/// display that leaves one refresh without a new frame every few seconds, which shows as a
/// stutter in a 30 fps game. Running 0.3% faster matches the display exactly. 1 otherwise.
double PaceScale();

/// Scale applied to the top screen's 2D art (dialogs, HUD) so it keeps its proportions when
/// the 400x240 image is shown at another aspect ratio: {x, y}, one of them 1.
std::pair<float, float> ArtScale();

} // namespace Leaf
