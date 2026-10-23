// Copyright 2026 Poly
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

package org.citra.citra_emu.leaf

/** Native side of the Animal Crossing: New Leaf integration (see src/core/leaf). */
object LeafNative {
    /** Enables the single-screen layout and sets how the main screen is fitted. */
    external fun setLayout(enabled: Boolean, displayMode: Int)

    /** Camera aspect (width / height) used by the game; 0 keeps the original 5:3. */
    external fun setCameraAspect(aspect: Float)

    /** Auto-stack on pickup; applied when the game boots (see src/core/leaf/pocket_stacks.h). */
    external fun setAutoStack(enabled: Boolean)

    external fun cameraAspectFor(displayMode: Int, width: Int, height: Int): Float

    external fun setPanelStyle(scale: Float, opacity: Float, anchor: Int)

    /** Display refresh rate (Hz), so emulation is paced to it. */
    external fun setDisplayRefresh(hz: Float)

    external fun setPanelVisible(visible: Boolean)

    external fun isPanelVisible(): Boolean

    /** Touches the 3DS bottom screen at normalized (0..1) coordinates. */
    external fun touchNormalized(x: Float, y: Float, pressed: Boolean)

    external fun isLeafTitle(titleId: Long): Boolean

    external fun cameraPatchAvailable(): Boolean

    /** True while the given remaster scene ("keyboard", "choice", ...) is on screen. */
    external fun sceneVisible(id: String): Boolean

    /**
     * Leaf's own buttons on screen: {icon, l, t, r, b, corner radius, corner shade} per button
     * (window pixels).
     */
    external fun buttons(): FloatArray

    /** Window position {x, y} of a bottom-screen point (320x240 pixels), or null. */
    external fun bottomToWindow(x: Float, y: Float): FloatArray?

    /** The game's pointing-hand cursor from the user's ROM, 32x32 ARGB, or null. */
    external fun getHandCursor(pressed: Boolean): IntArray?

    /** A pockets-screen item icon (icn_<index>) from the user's ROM, 32x32 ARGB, or null. */
    external fun getItemIcon(index: Int): IntArray?

    /**
     * The tool ring's tools: {slot, kind, variant, icon} each, slot -1 = in hand. Empty while
     * the ring can't be used (the player isn't standing still in the open).
     */
    external fun toolRing(): IntArray

    /** The tool ring is shown: the HUD (Leaf's and the game's clock) is left out meanwhile. */
    external fun setToolRingOpen(open: Boolean)

    /** Takes out the tool in pocket [slot], or puts away the one in hand for -1. */
    external fun selectTool(slot: Int)
}
