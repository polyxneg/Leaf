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

    /** Window position {x, y} of a bottom-screen point (320x240 pixels), or null. */
    external fun bottomToWindow(x: Float, y: Float): FloatArray?

    /** The game's pointing-hand cursor from the user's ROM, 32x32 ARGB, or null. */
    external fun getHandCursor(pressed: Boolean): IntArray?
}
