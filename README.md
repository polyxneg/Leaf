# Leaf

**Leaf** is a single-screen Android app for playing your own copy of
*Animal Crossing: New Leaf* on phones and handhelds. The game runs on one
screen at 16:9 or 4:3, and the bottom-screen menus are placed onto the main
screen as touchable elements, the way a remaster would show them.

> **Leaf is based on [Azahar](https://github.com/azahar-emu/azahar)**, the
> open-source Nintendo 3DS emulator that continues the legacy of Citra. All
> of the emulation (CPU, GPU, audio, system services) is Azahar's work. Leaf
> only adds a game-specific layer on top. Huge thanks to the Azahar and Citra
> contributors. Azahar's original README is kept in
> [AZAHAR-README.md](AZAHAR-README.md).

Leaf is an independent fork. It is **not affiliated with or endorsed by the
Azahar project or Nintendo**. Please report Leaf problems here, not to Azahar.

## Features

- **One screen, proper aspect ratios.** Choose 16:9, 4:3, the original 5:3 or
  automatic. The 3D camera is widened instead of stretched, and the 2D art
  (dialog boxes, the clock, the logo, the iris wipe) keeps its proportions.
- **Bottom-screen UI on the main screen.** You only see the elements you
  need, each placed where it makes sense:
  - choice bubbles docked at the corner of the dialog box;
  - a town HUD with menu tabs, minimap and chat button, which appears with
    the game's clock and fades out while you walk; a button on the minimap
    shows the live map large (you can keep walking) until you close it;
  - the text keyboard taking over the screen until you confirm;
  - the pockets and other menus filling the screen;
  - maps shown during conversations placed above the dialog;
  - the bulletin board with the memo in full view, its arrows beside it and
    Erase, Close and Write in a row below;
  - Welcome amiibo's screens: the coupon machine, and the passport's cat
    tasks as a full list whose details open on top when you pick a task.
- **Touch only where it makes sense.** Only the placed UI elements respond to
  touch, so there are no ghost touches on the rest of the screen.
- **Handheld controls.** Hardware controls are mapped automatically, and the
  on-screen controls hide when a gamepad is connected. A D-pad pointer lets
  you type on the keyboard without touching the screen.
- **Smooth play.** The emulator's pacing matches your display's refresh rate,
  the game gets extra emulated CPU headroom, and shaders compile in the
  background. Rendering can be set to native, 2x or 3x resolution.
- **Optional: stack items when picking them up** (off by default). Fruit,
  turnips, bell bags and stationery go onto a matching stack in your pockets,
  like in newer Animal Crossing games, and a full pocket still takes an item
  that fits on a stack. Leaf only uses the stacks the game already has (the
  ones you get by dragging items together), so saves contain nothing the
  unmodified game wouldn't write itself. Works with the original release and
  with Welcome amiibo (tested with the European versions); if your version
  isn't recognised, the option simply does nothing.

## Requirements

- An Android device (arm64, Android 10 or newer) with a reasonably fast GPU.
  Development and testing were done on a Retroid Pocket Nova.
- **Your own legally dumped, decrypted copy of Animal Crossing: New Leaf.**
  No game files are included or downloaded. Everything Leaf changes in the
  game's UI is generated on your device, from your copy, when the game
  starts. Leaf accepts the retail New Leaf and Welcome amiibo title IDs for
  every region, but testing was done with the European release.

## Installation

1. Download the APK from the [Releases](../../releases) page and install it.
2. Open Leaf, choose a folder for your save data, and pick your game file.
3. Press **Play**.

## Building

Leaf builds like Azahar's Android app (see [AZAHAR-README.md](AZAHAR-README.md)
and Azahar's documentation for the toolchain). From `src/android`:

```bash
./gradlew assembleVanillaRelWithDebInfo
```

For a signed release, set `ANDROID_KEYSTORE_FILE`, `ANDROID_KEY_ALIAS`,
`ANDROID_KEYSTORE_PASS` and, optionally, `LEAF_VERSION` (for example `1.0.0`),
then run `./gradlew assembleVanillaRelease`. Never commit your keystore or its
password; `.gitignore` excludes them.

The Leaf-specific code lives in:

- `src/core/leaf/`: game detection, UI layout patches, screen layout and
  scene detection;
- `src/android/app/src/main/java/org/citra/citra_emu/leaf/` and
  `src/android/app/src/main/jni/leaf_jni.cpp`: the Android launcher, settings
  and controls;
- `src/leaf_harness/`: a headless desktop tool used for development;
- small hooks in existing Azahar files (renderers, GPU, present shaders, input).

## AI-assisted development

The Leaf-specific code (the folders above and Leaf's changes to existing
Azahar files) was written with AI assistance (Anthropic's Claude) and reviewed
and tested by me. Azahar's [AI policy](https://github.com/azahar-emu/azahar/blob/master/AI-POLICY.md)
does not allow contributions like this, so **please do not submit Leaf code
to the Azahar project**.

## License

Leaf is licensed under the **GNU General Public License v2.0 or later**, the
same as Azahar. See [LICENSE.txt](LICENSE.txt). Files written for Leaf carry
the "Copyright 2026 Poly" notice; all other files keep their original
copyright notices.

*Animal Crossing, New Leaf and Nintendo 3DS are trademarks of Nintendo. Leaf
is not affiliated with, endorsed by or sponsored by Nintendo.*
