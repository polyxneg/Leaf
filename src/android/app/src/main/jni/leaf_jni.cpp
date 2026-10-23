// Copyright 2026 Poly
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// JNI bridge for org.citra.citra_emu.leaf.LeafNative (Animal Crossing: New Leaf integration).

#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include <fmt/format.h>
#include <jni.h>
#include "core/core.h"
#include "core/file_sys/romfs_reader.h"
#include "core/leaf/layout_patch.h"
#include "core/leaf/leaf.h"
#include "core/leaf/tool_ring.h"
#include "core/leaf/ui_scenes.h"
#include "core/loader/loader.h"
#include "video_core/texture/texture_decode.h"
#include "common/android_utils.h"
#include "jni/emu_window/emu_window.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"

EmuWindow_Android* GetLeafRenderWindow();

namespace {

void RefreshLayout() {
    auto& system = Core::System::GetInstance();
    if (system.IsPoweredOn()) {
        system.GPU().Renderer().UpdateCurrentFramebufferLayout(AndroidUtils::IsPortraitMode());
    }
}

/// An ETC1A4 image from a layout archive in the user's ROM as ARGB (top row first), or null.
/// Most layout images are stored bottom-up; `flip` turns them the right way up.
jintArray RomImage(JNIEnv* env, const char* archive, const std::string& path, bool flip) {
    auto& system = Core::System::GetInstance();
    if (!system.IsPoweredOn())
        return nullptr;
    std::shared_ptr<FileSys::RomFSReader> romfs;
    if (system.GetAppLoader().ReadRomFS(romfs) != Loader::ResultStatus::Success || !romfs)
        return nullptr;
    namespace LP = Leaf::LayoutPatch;
    const auto files = LP::ListRomFS(*romfs);
    const auto arc_it = files.find(archive);
    if (arc_it == files.end())
        return nullptr;
    const auto entries = LP::ReadArcEntries(LP::ReadRomFSFile(*romfs, arc_it->second));
    for (const auto& entry : entries) {
        if (entry.path != path)
            continue;
        const auto info = LP::ReadBclimInfo(entry.data.data(), static_cast<u32>(entry.data.size()));
        if (!info || info->format != 11) // ETC1A4
            return nullptr;
        Pica::Texture::TextureInfo tex{};
        tex.width = info->width;
        tex.height = info->height;
        tex.format = Pica::TexturingRegs::TextureFormat::ETC1A4;
        tex.SetDefaultStride();
        std::vector<jint> argb(static_cast<size_t>(info->width) * info->height);
        for (u32 y = 0; y < info->height; y++) {
            for (u32 x = 0; x < info->width; x++) {
                const auto c = Pica::Texture::LookupTexture(
                    entry.data.data(), x, flip ? info->height - 1 - y : y, tex);
                argb[y * info->width + x] =
                    static_cast<jint>((static_cast<u32>(c.a()) << 24) |
                                      (static_cast<u32>(c.r()) << 16) |
                                      (static_cast<u32>(c.g()) << 8) | c.b());
            }
        }
        jintArray result = env->NewIntArray(static_cast<jsize>(argb.size()));
        env->SetIntArrayRegion(result, 0, static_cast<jsize>(argb.size()), argb.data());
        return result;
    }
    return nullptr;
}

} // namespace

extern "C" {

JNIEXPORT void JNICALL Java_org_citra_citra_1emu_leaf_LeafNative_setLayout(
    JNIEnv*, jobject, jboolean enabled, jint display_mode) {
    auto& config = Leaf::GetConfig();
    config.layout_enabled = enabled;
    config.display_mode = display_mode;
    RefreshLayout();
}

JNIEXPORT void JNICALL Java_org_citra_citra_1emu_leaf_LeafNative_setCameraAspect(JNIEnv*, jobject,
                                                                                  jfloat aspect) {
    Leaf::GetConfig().camera_aspect = aspect;
    auto& system = Core::System::GetInstance();
    if (system.IsPoweredOn()) {
        Leaf::ApplyCameraAspect(system);
    }
}

JNIEXPORT void JNICALL Java_org_citra_citra_1emu_leaf_LeafNative_setAutoStack(JNIEnv*, jobject,
                                                                               jboolean enabled) {
    // Read when the game boots (and after savestate loads); set before starting a game.
    Leaf::GetConfig().auto_stack = enabled;
}

JNIEXPORT jfloat JNICALL Java_org_citra_citra_1emu_leaf_LeafNative_cameraAspectFor(
    JNIEnv*, jobject, jint display_mode, jint width, jint height) {
    return Leaf::CameraAspectFor(static_cast<Leaf::DisplayMode>(display_mode),
                                 static_cast<u32>(width), static_cast<u32>(height));
}

JNIEXPORT void JNICALL Java_org_citra_citra_1emu_leaf_LeafNative_setDisplayRefresh(JNIEnv*,
                                                                                    jobject,
                                                                                    jfloat hz) {
    Leaf::GetConfig().display_refresh = hz;
}

JNIEXPORT void JNICALL Java_org_citra_citra_1emu_leaf_LeafNative_setPanelStyle(
    JNIEnv*, jobject, jfloat scale, jfloat opacity, jint anchor) {
    auto& config = Leaf::GetConfig();
    config.panel_scale = scale;
    config.panel_opacity = opacity;
    config.panel_anchor = anchor;
    RefreshLayout();
}

JNIEXPORT void JNICALL Java_org_citra_citra_1emu_leaf_LeafNative_setPanelVisible(JNIEnv*, jobject,
                                                                                  jboolean visible) {
    Leaf::GetConfig().panel_visible = visible;
    if (auto* window = GetLeafRenderWindow(); !visible && window) {
        window->TouchReleased();
    }
    RefreshLayout();
}

JNIEXPORT jboolean JNICALL Java_org_citra_citra_1emu_leaf_LeafNative_isPanelVisible(JNIEnv*,
                                                                                     jobject) {
    return Leaf::GetConfig().panel_visible.load();
}

/// Touch at normalized bottom-screen coordinates (0..1), whether or not the panel is shown.
JNIEXPORT void JNICALL Java_org_citra_citra_1emu_leaf_LeafNative_touchNormalized(
    JNIEnv*, jobject, jfloat x, jfloat y, jboolean pressed) {
    auto* window = GetLeafRenderWindow();
    if (!window)
        return;
    if (pressed) {
        window->TouchPressedNormalized(x, y);
    } else {
        window->TouchReleased();
    }
}

JNIEXPORT jboolean JNICALL Java_org_citra_citra_1emu_leaf_LeafNative_isLeafTitle(JNIEnv*, jobject,
                                                                                 jlong title_id) {
    return Leaf::IsLeafTitle(static_cast<u64>(title_id));
}

JNIEXPORT jboolean JNICALL Java_org_citra_citra_1emu_leaf_LeafNative_cameraPatchAvailable(JNIEnv*,
                                                                                         jobject) {
    return Leaf::CameraPatchAvailable();
}

JNIEXPORT jboolean JNICALL Java_org_citra_citra_1emu_leaf_LeafNative_sceneVisible(JNIEnv* env,
                                                                                  jobject,
                                                                                  jstring id) {
    const char* chars = env->GetStringUTFChars(id, nullptr);
    const bool visible = Leaf::Ui::SceneVisible(chars);
    env->ReleaseStringUTFChars(id, chars);
    return visible;
}

/// Leaf's own buttons on screen as {icon, left, top, right, bottom, corner radius, corner shade}
/// * n (window pixels).
JNIEXPORT jfloatArray JNICALL Java_org_citra_citra_1emu_leaf_LeafNative_buttons(JNIEnv* env,
                                                                                jobject) {
    auto* window = GetLeafRenderWindow();
    if (!window)
        return env->NewFloatArray(0);
    const auto& layout = window->GetFramebufferLayout();
    std::vector<jfloat> values;
    for (const auto& button : Leaf::Ui::Buttons(layout.width, layout.height)) {
        values.insert(values.end(), {static_cast<jfloat>(button.icon),
                                     static_cast<jfloat>(button.rect.left),
                                     static_cast<jfloat>(button.rect.top),
                                     static_cast<jfloat>(button.rect.right),
                                     static_cast<jfloat>(button.rect.bottom),
                                     button.corner_radius, button.corner_shade});
    }
    jfloatArray result = env->NewFloatArray(static_cast<jsize>(values.size()));
    env->SetFloatArrayRegion(result, 0, static_cast<jsize>(values.size()), values.data());
    return result;
}

/// Window position of a bottom-screen point as {x, y}, or null when it is not on screen.
JNIEXPORT jfloatArray JNICALL Java_org_citra_citra_1emu_leaf_LeafNative_bottomToWindow(
    JNIEnv* env, jobject, jfloat x, jfloat y) {
    auto* window = GetLeafRenderWindow();
    if (!window)
        return nullptr;
    const auto& layout = window->GetFramebufferLayout();
    float wx, wy;
    if (!Leaf::Ui::BottomToWindow(layout.width, layout.height, x, y, wx, wy))
        return nullptr;
    jfloatArray result = env->NewFloatArray(2);
    const jfloat values[2] = {wx, wy};
    env->SetFloatArrayRegion(result, 0, 2, values);
    return result;
}

/// The game's own pointing-hand cursor (from the user's ROM) as 32x32 ARGB, or null. `pressed`
/// selects the variant with the finger pushed down.
JNIEXPORT jintArray JNICALL Java_org_citra_citra_1emu_leaf_LeafNative_getHandCursor(
    JNIEnv* env, jobject, jboolean pressed) {
    return RomImage(env, "Layout/Cursor/crsl_hand.arc",
                    pressed ? "timg/tc_crsl_hand_c.bclim" : "timg/tc_crsl_hand_po.bclim", true);
}

/// An item icon of the pockets screen (ItemWin/item_icon.arc icn_<index>) as 32x32 ARGB, or null.
JNIEXPORT jintArray JNICALL Java_org_citra_citra_1emu_leaf_LeafNative_getItemIcon(
    JNIEnv* env, jobject, jint index) {
    return RomImage(env, "Layout/ItemWin/item_icon.arc",
                    fmt::format("timg/icn_{:03d}.bclim", index), false);
}

/// The tool ring's tools as {slot, kind, variant, icon} each (slot -1 = in hand); empty while the
/// ring can't be used (the player isn't standing still, or a menu is open).
JNIEXPORT jintArray JNICALL Java_org_citra_citra_1emu_leaf_LeafNative_toolRing(JNIEnv* env,
                                                                               jobject) {
    std::vector<jint> values;
    const auto scenes = Leaf::Ui::ActiveScenes();
    if (std::all_of(scenes.begin(), scenes.end(),
                    [](const std::string& id) { return id == "town_hud"; })) {
        for (const auto& tool : Leaf::ToolRing::Tools())
            values.insert(values.end(), {tool.slot, tool.kind, tool.variant, tool.icon});
    }
    jintArray result = env->NewIntArray(static_cast<jsize>(values.size()));
    env->SetIntArrayRegion(result, 0, static_cast<jsize>(values.size()), values.data());
    return result;
}

/// The tool ring is shown (the HUD is left out meanwhile).
JNIEXPORT void JNICALL Java_org_citra_citra_1emu_leaf_LeafNative_setToolRingOpen(JNIEnv*, jobject,
                                                                                jboolean open) {
    Leaf::GetConfig().tool_ring_open = open;
}

/// Takes out the tool in pocket `slot`, or puts away the one in hand for -1.
JNIEXPORT void JNICALL Java_org_citra_citra_1emu_leaf_LeafNative_selectTool(JNIEnv*, jobject,
                                                                           jint slot) {
    Leaf::ToolRing::Select(slot);
}

} // extern "C"
