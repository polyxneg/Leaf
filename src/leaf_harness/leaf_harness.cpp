// Copyright 2026 Poly
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Headless desktop harness used while developing the Leaf (Animal Crossing: New Leaf) Android
// port. Boots a user-supplied ROM with the software renderer and executes a small command script
// so screens can be captured, memory inspected and game patches verified without a device.
//
// Script commands (one per line, '#' starts a comment):
//   run <frames>                      advance emulation
//   hold <buttons> <frames>           hold buttons (comma separated: a,b,x,y,up,down,left,right,
//                                     l,r,start,select,zl,zr) for N frames, then release
//   stick <x> <y> <frames>            circle pad (-1..1) for N frames
//   tap <x> <y> [frames]              touch bottom screen at native 320x240 coords
//   drag <x1> <y1> <x2> <y2> [frames] touch-drag on the bottom screen over N frames
//   shot <name>                       write <out>/<name>_top.png and <name>_bot.png
//   r32 <addr> / w32 <addr> <value>   read / write emulated memory
//   wf32 <addr> <float>               write a float
//   dump <addr> <len> <file>          dump memory to a file
//   save <file> / load <file>         savestates
//   aspect <ratio>                    apply Leaf camera aspect (0 = native, e.g. 1.7778)
//   autostack <0|1>                   Leaf auto-stack on pickup (LEAF_AUTOSTACK=1 at boot)
//   ring / ringsel <slot>             list the tool ring / take out a slot (-1 = put away)
//   ringopen <0|1>                    tool ring shown (HUD left out)
//   w8 / w16 <addr> <value>           write a byte / halfword
//   hexd <addr> <len>                 hex dump to stdout
//   find <hexbytes> [max]             search all mapped process memory for a byte pattern
//   snap <file>                       dump every writable mapping as (u32 addr, u32 size, data)
// Reverse-engineering commands (need LEAF_INTERP=1 and a LEAF_DEBUG_HOOKS build):
//   watch <addr> <len> [max]          log writes into [addr, addr+len) with registers
//   rwatch <addr> <len> [max]         log reads from [addr, addr+len) (pc, lr)
//   bp <addr>[,<addr>...] [max]       log registers whenever an instruction address executes
//   unwatch / unbp                    remove them

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <lodepng.h>

#include "common/common_paths.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/frontend/input.h"
#include "core/hle/service/service.h"
#include "core/hle/kernel/process.h"
#include "core/leaf/debug_hooks.h"
#include "core/leaf/leaf.h"
#include "core/leaf/pocket_stacks.h"
#include "core/leaf/tool_ring.h"
#include "core/leaf/ui_patcher.h"
#include "core/leaf/ui_probe.h"
#include "core/leaf/ui_scenes.h"
#include "core/memory.h"
#include "video_core/gpu.h"
#include "video_core/renderer_software/renderer_software.h"

namespace {

std::array<bool, Settings::NativeButton::NumButtons> g_buttons{};
float g_stick_x = 0.f, g_stick_y = 0.f;
u64 g_frames = 0;

class HarnessButton final : public Input::ButtonDevice {
public:
    explicit HarnessButton(int id_) : id(id_) {}
    bool GetStatus() const override {
        return g_buttons[id];
    }

private:
    int id;
};

class HarnessButtonFactory final : public Input::Factory<Input::ButtonDevice> {
public:
    std::unique_ptr<Input::ButtonDevice> Create(const Common::ParamPackage& params) override {
        return std::make_unique<HarnessButton>(params.Get("code", 0));
    }
};

class HarnessStick final : public Input::AnalogDevice {
public:
    std::tuple<float, float> GetStatus() const override {
        return {g_stick_x, g_stick_y};
    }
};

class HarnessStickFactory final : public Input::Factory<Input::AnalogDevice> {
public:
    std::unique_ptr<Input::AnalogDevice> Create(const Common::ParamPackage&) override {
        return std::make_unique<HarnessStick>();
    }
};

class HarnessWindow final : public Frontend::EmuWindow {
public:
    HarnessWindow() {
        // Stacked native layout: top 400x240 at y=0, bottom 320x240 at x=40,y=240.
        UpdateCurrentFramebufferLayout(400, 480);
    }
    void PollEvents() override {
        g_frames++;
    }
    void SwapBuffers() override {}
    void MakeCurrent() override {}
    void DoneCurrent() override {}
};

int ButtonId(const std::string& name) {
    using namespace Settings::NativeButton;
    static const std::vector<std::pair<std::string, int>> map = {
        {"a", A},          {"b", B},          {"x", X},           {"y", Y},
        {"up", Up},        {"down", Down},    {"left", Left},     {"right", Right},
        {"l", L},          {"r", R},          {"start", Start},   {"select", Select},
        {"zl", ZL},        {"zr", ZR},        {"home", Home},
    };
    for (const auto& [n, id] : map) {
        if (n == name)
            return id;
    }
    return -1;
}

/// Converts a portrait, column-major ScreenInfo into landscape RGBA rows.
std::vector<u8> ToLandscape(const SwRenderer::ScreenInfo& info, u32& w, u32& h) {
    w = info.height;
    h = std::min<u32>(info.width, 240);
    const u32 pad = info.width - h;
    std::vector<u8> out(static_cast<size_t>(w) * h * 4);
    for (u32 y = 0; y < h; y++) {
        for (u32 x = 0; x < w; x++) {
            const u8* src =
                info.pixels.data() + (static_cast<size_t>(y + pad) * info.height + x) * 4;
            u8* dst = out.data() + (static_cast<size_t>(y) * w + x) * 4;
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = 255;
        }
    }
    return out;
}

/// Writes both screens stacked like a 3DS (top 400x240 above a centred 320x240 bottom).
bool WriteStacked(const std::string& path, const SwRenderer::ScreenInfo& top,
                  const SwRenderer::ScreenInfo& bottom) {
    if (top.pixels.empty() || bottom.pixels.empty())
        return false;
    u32 tw, th, bw, bh;
    const auto t = ToLandscape(top, tw, th);
    const auto b = ToLandscape(bottom, bw, bh);
    std::vector<u8> out(static_cast<size_t>(tw) * (th + bh) * 4, 0);
    for (u32 y = 0; y < th; y++)
        std::memcpy(&out[y * tw * 4], &t[y * tw * 4], tw * 4);
    const u32 xo = (tw - bw) / 2;
    for (u32 y = 0; y < bh; y++)
        std::memcpy(&out[((th + y) * tw + xo) * 4], &b[y * bw * 4], bw * 4);
    return lodepng::encode(path, out, tw, th + bh) == 0;
}

float Smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// Bilinear sample of a landscape RGBA image at normalized coordinates.
void Sample(const std::vector<u8>& img, u32 w, u32 h, float u, float v, float out[4]) {
    const float x = std::clamp(u * w - 0.5f, 0.0f, w - 1.0f);
    const float y = std::clamp(v * h - 0.5f, 0.0f, h - 1.0f);
    const u32 x0 = static_cast<u32>(x), y0 = static_cast<u32>(y);
    const u32 x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const float fx = x - x0, fy = y - y0;
    for (int c = 0; c < 4; c++) {
        auto p = [&](u32 px, u32 py) { return img[(py * w + px) * 4 + c] / 255.0f; };
        out[c] = (p(x0, y0) * (1 - fx) + p(x1, y0) * fx) * (1 - fy) +
                 (p(x0, y1) * (1 - fx) + p(x1, y1) * fx) * fy;
    }
}

/// Software version of the Leaf presentation (same maths as the present shaders), so layouts
/// can be reviewed without a device.
bool WriteComposite(const std::string& path, const SwRenderer::ScreenInfo& top_info,
                    const SwRenderer::ScreenInfo& bottom_info, u32 width, u32 height) {
    u32 tw, th, bw, bh;
    const auto top = ToLandscape(top_info, tw, th);
    const auto bottom = ToLandscape(bottom_info, bw, bh);
    const auto layout = Layout::LeafFrameLayout(width, height);
    std::vector<float> out(static_cast<size_t>(width) * height * 3, 0.0f);
    const auto& tr = layout.top_screen;
    for (u32 y = tr.top; y < tr.bottom; y++) {
        for (u32 x = tr.left; x < tr.right; x++) {
            float c[4];
            const auto* crop = Leaf::TopScreenCrop();
            const float v = (y - tr.top + 0.5f) / tr.GetHeight();
            Sample(top, tw, th, (x - tr.left + 0.5f) / tr.GetWidth(),
                   crop ? crop->top + (crop->bottom - crop->top) * v : v, c);
            std::copy(c, c + 3, &out[(y * width + x) * 3]);
        }
    }
    // Settle the scene fades, then draw the regions like the GPU does.
    std::vector<Layout::ScreenRegion> regions;
    for (int i = 0; i < 20; i++)
        regions = Leaf::Ui::FrameRegions(layout, 0.05f);
    // Same maths as the present shaders: key every texel, then filter premultiplied results.
    const auto key_texel = [&](const Layout::ScreenRegion& r, int tx, int ty, float out[4]) {
        // Regions normally show the bottom screen; `from_top` ones a part of the top screen.
        const auto& image = r.from_top ? top : bottom;
        const u32 iw = r.from_top ? tw : bw, ih = r.from_top ? th : bh;
        tx = std::clamp(tx, 0, static_cast<int>(iw) - 1);
        ty = std::clamp(ty, 0, static_cast<int>(ih) - 1);
        float c[3];
        for (int k = 0; k < 3; k++)
            c[k] = image[(static_cast<size_t>(ty) * iw + tx) * 4 + k] / 255.0f;
        float a = 1.0f;
        if (r.key_tolerance > 0.0f) {
            a = 1.0f - std::clamp(std::min(c[0], c[2]) - c[1], 0.0f, 1.0f);
            const float inv = 1.0f / std::max(a, 0.001f);
            c[0] = std::clamp((c[0] - (1 - a)) * inv, 0.0f, 1.0f);
            c[1] = std::clamp(c[1] * inv, 0.0f, 1.0f);
            c[2] = std::clamp((c[2] - (1 - a)) * inv, 0.0f, 1.0f);
            const float d = std::sqrt((c[0] - 0.54f) * (c[0] - 0.54f) +
                                      (c[1] - 0.54f) * (c[1] - 0.54f) +
                                      (c[2] - 0.885f) * (c[2] - 0.885f));
            const float rim = 1.0f - Smoothstep(0.06f, 0.14f, d);
            const float shadow[3] = {0.06f, 0.05f, 0.09f};
            for (int k = 0; k < 3; k++)
                c[k] += (shadow[k] - c[k]) * rim;
            a *= 1.0f + (0.4f - 1.0f) * rim;
            a *= Smoothstep(r.key_tolerance, r.key_tolerance + r.key_softness, a);
        }
        for (int k = 0; k < 3; k++)
            out[k] = c[k] * a;
        out[3] = a;
    };
    for (const auto& r : regions) {
        if (r.key_tolerance < 0.0f) { // backdrop
            const float a = r.scrim * r.opacity;
            for (u32 y = r.dst.top; y < std::min(r.dst.bottom, height); y++)
                for (u32 x = r.dst.left; x < std::min(r.dst.right, width); x++)
                    for (int k = 0; k < 3; k++)
                        out[(y * width + x) * 3 + k] *= 1.0f - a;
            continue;
        }
        for (u32 y = r.dst.top; y < std::min(r.dst.bottom, height); y++) {
            for (u32 x = r.dst.left; x < std::min(r.dst.right, width); x++) {
                const float fx = (x - r.dst.left + 0.5f) / r.dst.GetWidth();
                const float fy = (y - r.dst.top + 0.5f) / r.dst.GetHeight();
                const float px =
                    (r.src.left + fx * (r.src.right - r.src.left)) * (r.from_top ? tw : bw) - 0.5f;
                const float py =
                    (r.src.top + fy * (r.src.bottom - r.src.top)) * (r.from_top ? th : bh) - 0.5f;
                const int ix = static_cast<int>(std::floor(px)), iy = static_cast<int>(std::floor(py));
                const float tx = px - ix, ty = py - iy;
                float c00[4], c10[4], c01[4], c11[4], pm[4];
                key_texel(r, ix, iy, c00);
                key_texel(r, ix + 1, iy, c10);
                key_texel(r, ix, iy + 1, c01);
                key_texel(r, ix + 1, iy + 1, c11);
                for (int k = 0; k < 4; k++)
                    pm[k] = (c00[k] * (1 - tx) + c10[k] * tx) * (1 - ty) +
                            (c01[k] * (1 - tx) + c11[k] * tx) * ty;
                const float a = pm[3];
                const float total = 1.0f - (1.0f - a) * (1.0f - r.scrim);
                const float out_a = total * r.opacity;
                float* o = &out[(y * width + x) * 3];
                for (int k = 0; k < 3; k++) {
                    const float col = a > 0.0001f ? pm[k] / a : 0.0f;
                    o[k] = col * a / std::max(total, 0.001f) * out_a + o[k] * (1 - out_a);
                }
            }
        }
    }
    // Leaf's own buttons (drawn by the frontend on a device): a cream disc with a brown rim;
    // rounded-corner masks dim the corners outside the rounded rectangle.
    for (const auto& r : regions) {
        if (r.icon == Leaf::Ui::IconRoundCorners) {
            const float rad = r.corner_radius;
            for (u32 y = r.dst.top; y < std::min(r.dst.bottom, height); y++) {
                for (u32 x = r.dst.left; x < std::min(r.dst.right, width); x++) {
                    const float cx = std::clamp(x + 0.5f, r.dst.left + rad, r.dst.right - rad);
                    const float cy = std::clamp(y + 0.5f, r.dst.top + rad, r.dst.bottom - rad);
                    if (std::hypot(x + 0.5f - cx, y + 0.5f - cy) > rad)
                        for (int k = 0; k < 3; k++)
                            out[(y * width + x) * 3 + k] *= 1.0f - r.corner_shade;
                }
            }
            continue;
        }
        if (r.icon == 0 || !r.touchable)
            continue;
        const float cx = (r.dst.left + r.dst.right) / 2.0f, cy = (r.dst.top + r.dst.bottom) / 2.0f;
        const float rad = std::min(r.dst.GetWidth(), r.dst.GetHeight()) / 2.0f;
        for (u32 y = r.dst.top; y < std::min(r.dst.bottom, height); y++) {
            for (u32 x = r.dst.left; x < std::min(r.dst.right, width); x++) {
                const float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
                if (d > rad)
                    continue;
                const bool rim = d > rad * 0.86f;
                const float c[3] = {rim ? 0.47f : 1.0f, rim ? 0.34f : 0.98f, rim ? 0.2f : 0.92f};
                std::copy(c, c + 3, &out[(y * width + x) * 3]);
            }
        }
    }
    std::vector<u8> png(static_cast<size_t>(width) * height * 4, 255);
    for (size_t i = 0; i < static_cast<size_t>(width) * height; i++)
        for (int k = 0; k < 3; k++)
            png[i * 4 + k] = static_cast<u8>(std::clamp(out[i * 3 + k], 0.0f, 1.0f) * 255.0f);
    return lodepng::encode(path, png, width, height) == 0;
}

bool WritePng(const std::string& path, const SwRenderer::ScreenInfo& info) {
    if (info.pixels.empty())
        return false;
    // ScreenInfo is portrait/column-major; transpose to landscape.
    // The portrait buffer is padded to 256 columns; the visible 240 rows are the last ones.
    const u32 w = info.height, h = std::min<u32>(info.width, 240), pad = info.width - h;
    std::vector<u8> out(static_cast<size_t>(w) * h * 4);
    for (u32 y = 0; y < h; y++) {
        for (u32 x = 0; x < w; x++) {
            const u8* src =
                info.pixels.data() + (static_cast<size_t>(y + pad) * info.height + x) * 4;
            u8* dst = out.data() + (static_cast<size_t>(y) * w + x) * 4;
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = 255;
        }
    }
    return lodepng::encode(path, out, w, h) == 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <rom> <script|-> <outdir> [userdir]\n", argv[0]);
        return 1;
    }
    const std::string rom = argv[1];
    const std::string script = argv[2];
    const std::string outdir = argv[3];
    if (argc > 4)
        FileUtil::SetUserPath(std::string(argv[4]) + "/");
    else
        FileUtil::SetUserPath(outdir + "/user/");

    Common::Log::Filter filter(Common::Log::Level::Warning);
    Common::Log::Initialize();
    filter.ParseFilterString("*:Warning Core:Info");
    Common::Log::SetGlobalFilter(filter);
    Common::Log::Start();

    for (const auto& service_module : Service::service_module_map)
        Settings::values.lle_modules.emplace(service_module.name, false);
    Settings::values.graphics_api = Settings::GraphicsAPI::Software;
    Settings::values.frame_limit = 0;
    Settings::values.use_cpu_jit = true;
    Settings::values.init_clock = Settings::InitClock::FixedTime;
    Settings::values.init_time = 1767268800ULL; // 2026-01-01 12:00 UTC, keeps runs reproducible
    Settings::values.audio_emulation = Settings::AudioEmulation::HLE;
    Settings::values.output_type = AudioCore::SinkType::Null;
    if (std::getenv("LEAF_INTERP")) {
        Settings::values.use_cpu_jit = false;
        Settings::values.use_fastinterp = false;
    }
    if (std::getenv("LEAF_AUTOSTACK"))
        Leaf::GetConfig().auto_stack = true;
    if (const char* a = std::getenv("LEAF_ASPECT"))
        Leaf::GetConfig().camera_aspect = std::stof(a);

    Input::RegisterFactory<Input::ButtonDevice>("harness",
                                                std::make_shared<HarnessButtonFactory>());
    Input::RegisterFactory<Input::AnalogDevice>("harness", std::make_shared<HarnessStickFactory>());
    auto& profile = Settings::values.current_input_profile;
    for (int i = 0; i < Settings::NativeButton::NumButtons; i++)
        profile.buttons[i] = "engine:harness,code:" + std::to_string(i);
    for (auto& a : profile.analogs)
        a = "engine:harness";
    profile.touch_device = "engine:emu_window";
    profile.motion_device = "";


    HarnessWindow window;
    auto& system = Core::System::GetInstance();
    const auto status = system.Load(window, rom);
    if (status != Core::System::ResultStatus::Success) {
        std::fprintf(stderr, "load failed: %d\n", static_cast<int>(status));
        return 2;
    }

    auto run_frames = [&](u64 n) {
        const u64 target = g_frames + n;
        while (g_frames < target) {
            if (system.RunLoop() != Core::System::ResultStatus::Success) {
                std::fprintf(stderr, "RunLoop error: %s\n", system.GetStatusDetails().c_str());
                return false;
            }
        }
        return true;
    };

    std::ifstream file;
    std::istream* in = &std::cin;
    if (script != "-") {
        file.open(script);
        in = &file;
    }

    std::string line;
    while (std::getline(*in, line)) {
        if (auto c = line.find('#'); c != std::string::npos)
            line.resize(c);
        std::istringstream ss(line);
        std::string cmd;
        if (!(ss >> cmd))
            continue;
        if (cmd == "run") {
            u64 n;
            ss >> n;
            if (!run_frames(n))
                return 3;
        } else if (cmd == "hold") {
            std::string list;
            u64 n = 1;
            ss >> list >> n;
            std::stringstream ls(list);
            std::string b;
            std::vector<int> ids;
            while (std::getline(ls, b, ',')) {
                const int id = ButtonId(b);
                if (id < 0) {
                    std::fprintf(stderr, "unknown button %s\n", b.c_str());
                    continue;
                }
                ids.push_back(id);
                g_buttons[id] = true;
                if (b == "a")
                    Leaf::Ui::NotifyConfirm();
            }
            run_frames(n);
            for (int id : ids)
                g_buttons[id] = false;
        } else if (cmd == "stick") {
            u64 n;
            ss >> g_stick_x >> g_stick_y >> n;
            run_frames(n);
            g_stick_x = g_stick_y = 0.f;
        } else if (cmd == "tap") {
            unsigned x, y;
            u64 n = 6;
            ss >> x >> y >> n;
            window.TouchPressed(40 + x, 240 + y);
            run_frames(n);
            window.TouchReleased();
        } else if (cmd == "drag") {
            float x1, y1, x2, y2;
            u64 n = 30;
            ss >> x1 >> y1 >> x2 >> y2 >> n;
            window.TouchPressed(40 + static_cast<unsigned>(x1), 240 + static_cast<unsigned>(y1));
            run_frames(8);
            for (u64 i = 1; i <= n; i++) {
                const float t = static_cast<float>(i) / n;
                window.TouchMoved(40 + static_cast<unsigned>(x1 + (x2 - x1) * t),
                                  240 + static_cast<unsigned>(y1 + (y2 - y1) * t));
                run_frames(1);
            }
            run_frames(8);
            window.TouchReleased();
        } else if (cmd == "wtap") {
            // wtap <x> <y> <window w> <window h> [frames]: tap the main screen like a player
            // would, through the regions of the last composite of that size.
            unsigned x, y, w, h;
            u64 n = 6;
            ss >> x >> y >> w >> h >> n;
            float nx, ny;
            const auto regions = Leaf::Ui::LastRegions(w, h);
            const int region = Layout::MapRegionTouch(regions, x, y, nx, ny);
            std::printf("wtap region %d -> %.1f %.1f\n", region, nx * 320.0f, ny * 240.0f);
            // Overlay actions as in EmuWindow::TouchPressed.
            if (region >= 0 && (regions[region].action == 2 || regions[region].action == 3)) {
                Leaf::Ui::SetOverlayOpen(regions[region].action == 3);
                run_frames(n);
            } else if (region >= 0) {
                if (regions[region].action == 1)
                    Leaf::Ui::SetOverlayOpen(true);
                window.TouchPressedNormalized(nx, ny);
                run_frames(n);
                window.TouchReleased();
            }
        } else if (cmd == "shot") {
            std::string name;
            ss >> name;
            const auto& r = static_cast<SwRenderer::RendererSoftware&>(system.GPU().Renderer());
            WritePng(outdir + "/" + name + "_top.png", r.Screen(VideoCore::ScreenId::TopLeft));
            WritePng(outdir + "/" + name + "_bot.png", r.Screen(VideoCore::ScreenId::Bottom));
            WriteStacked(outdir + "/" + name + ".png", r.Screen(VideoCore::ScreenId::TopLeft),
                         r.Screen(VideoCore::ScreenId::Bottom));
            std::printf("shot %s at frame %llu\n", name.c_str(),
                        static_cast<unsigned long long>(g_frames));
        } else if (cmd == "r32") {
            std::string a;
            ss >> a;
            const VAddr addr = std::stoul(a, nullptr, 16);
            const u32 v = system.Memory().Read32(addr);
            float f;
            std::memcpy(&f, &v, 4);
            std::printf("[%08x] = %08x (%g)\n", addr, v, f);
        } else if (cmd == "w32" || cmd == "wf32") {
            std::string a, v;
            ss >> a >> v;
            const VAddr addr = std::stoul(a, nullptr, 16);
            u32 val;
            if (cmd == "wf32") {
                const float f = std::stof(v);
                std::memcpy(&val, &f, 4);
            } else {
                val = std::stoul(v, nullptr, 16);
            }
            system.Memory().Write32(addr, val);
        } else if (cmd == "dump") {
            std::string a, f;
            u32 len;
            ss >> a >> len >> f;
            std::vector<u8> buf(len);
            system.Memory().ReadBlock(std::stoul(a, nullptr, 16), buf.data(), len);
            std::ofstream(outdir + "/" + f, std::ios::binary)
                .write(reinterpret_cast<const char*>(buf.data()), len);
        } else if (cmd == "save") {
            std::string f;
            ss >> f;
            const auto buf = system.SaveStateBuffer();
            std::ofstream(f, std::ios::binary)
                .write(reinterpret_cast<const char*>(buf.data()), buf.size());
            std::printf("saved %zu bytes\n", buf.size());
        } else if (cmd == "load") {
            std::string f;
            ss >> f;
            std::ifstream st(f, std::ios::binary);
            std::vector<u8> buf((std::istreambuf_iterator<char>(st)), {});
            if (!system.LoadStateBuffer(std::move(buf)))
                std::fprintf(stderr, "load state failed\n");
        } else if (cmd == "aspect") {
            float a;
            ss >> a;
            Leaf::GetConfig().camera_aspect = a;
            Leaf::ApplyCameraAspect(system);
        } else if (cmd == "autostack") {
            int on = 0;
            ss >> on;
            Leaf::GetConfig().auto_stack = on != 0;
            Leaf::PocketStacks::Apply(system);
            std::printf("autostack %d (available %d)\n", on, Leaf::PocketStacks::Available());
        } else if (cmd == "ring") {
            const auto tools = Leaf::ToolRing::Tools();
            std::printf("ring (available %d):", Leaf::ToolRing::Available());
            for (const auto& t : tools)
                std::printf(" [slot %d kind %02x variant %d icon %d]", t.slot, t.kind, t.variant,
                            t.icon);
            std::printf("\n");
        } else if (cmd == "ringopen") {
            int open = 0;
            ss >> open;
            Leaf::GetConfig().tool_ring_open = open != 0;
        } else if (cmd == "ringsel") {
            int slot = -1;
            ss >> slot;
            Leaf::ToolRing::Select(slot);
        } else if (cmd == "composite") {
            // composite <name> <width> <height> <mode: 0=16:9 1=4:3 2=5:3 3=fill>
            std::string name;
            u32 w = 1280, h = 720;
            int mode = 0;
            ss >> name >> w >> h >> mode;
            Leaf::GetConfig().layout_enabled = true;
            Leaf::GetConfig().display_mode = mode;
            const auto& r = static_cast<SwRenderer::RendererSoftware&>(system.GPU().Renderer());
            WriteComposite(outdir + "/" + name + ".png", r.Screen(VideoCore::ScreenId::TopLeft),
                           r.Screen(VideoCore::ScreenId::Bottom), w, h);
            for (const auto& id : Leaf::Ui::ActiveScenes())
                std::printf("scene %s\n", id.c_str());
            if (std::getenv("LEAF_REGIONS"))
                for (const auto& rg : Leaf::Ui::LastRegions(w, h))
                    std::printf("region src %.1f %.1f %.1f %.1f dst %u %u %u %u a %.2f\n",
                                rg.src.left * (rg.from_top ? 400 : 320), rg.src.top * 240,
                                rg.src.right * (rg.from_top ? 400 : 320),
                                rg.src.bottom * 240, rg.dst.left, rg.dst.top, rg.dst.right,
                                rg.dst.bottom, rg.opacity);
        } else if (cmd == "probe") {
            std::printf("probe:");
            for (const u64 key : Leaf::UiProbe::LastFrameKeys())
                std::printf(" %016llx", static_cast<unsigned long long>(key));
            std::printf("\nprobe top:");
            for (const u64 key : Leaf::UiProbe::LastFrameTopKeys()) {
                const auto name = Leaf::Ui::TextureName(key);
                if (!name.empty())
                    std::printf(" %s", name.c_str());
            }
            std::printf("\n");
        } else if (cmd == "bounds") {
            // bounds: measured bottom-screen rectangles of the last frame's textures
            Leaf::UiProbe::SetWantBounds(true);
            for (const auto& [key, r] : Leaf::UiProbe::LastFrameBounds())
                std::printf("bounds %016llx %.1f %.1f %.1f %.1f\n",
                            static_cast<unsigned long long>(key), r.left, r.top, r.right,
                            r.bottom);
        } else if (cmd == "w8" || cmd == "w16") {
            std::string a, v;
            ss >> a >> v;
            const VAddr addr = std::stoul(a, nullptr, 16);
            const u32 val = std::stoul(v, nullptr, 16);
            if (cmd == "w8")
                system.Memory().Write8(addr, static_cast<u8>(val));
            else
                system.Memory().Write16(addr, static_cast<u16>(val));
        } else if (cmd == "hexd") {
            std::string a;
            u32 len = 64;
            ss >> a >> len;
            const VAddr addr = std::stoul(a, nullptr, 16);
            std::vector<u8> buf(len);
            system.Memory().ReadBlock(addr, buf.data(), len);
            for (u32 i = 0; i < len; i += 16) {
                std::printf("%08x:", addr + i);
                for (u32 j = i; j < std::min(len, i + 16); j++)
                    std::printf(" %02x", buf[j]);
                std::printf("\n");
            }
        } else if (cmd == "find" || cmd == "snap") {
            std::string arg;
            u32 max_hits = 64;
            ss >> arg >> max_hits;
            std::vector<u8> pattern;
            if (cmd == "find")
                for (std::size_t i = 0; i + 1 < arg.size(); i += 2)
                    pattern.push_back(static_cast<u8>(std::stoul(arg.substr(i, 2), nullptr, 16)));
            std::ofstream snap;
            if (cmd == "snap")
                snap.open(arg, std::ios::binary);
            auto process = system.Kernel().GetCurrentProcess();
            u32 hits = 0;
            for (const auto& [base, vma] : process->vm_manager.vma_map) {
                if (vma.type != Kernel::VMAType::BackingMemory ||
                    (static_cast<u32>(vma.permissions) & 1) == 0)
                    continue;
                std::vector<u8> buf(vma.size);
                system.Memory().ReadBlock(*process, base, buf.data(), vma.size);
                if (cmd == "snap") {
                    if ((static_cast<u32>(vma.permissions) & 2) == 0)
                        continue;
                    const u32 hdr[2] = {base, vma.size};
                    snap.write(reinterpret_cast<const char*>(hdr), 8);
                    snap.write(reinterpret_cast<const char*>(buf.data()), buf.size());
                    continue;
                }
                for (auto it = buf.begin();
                     (it = std::search(it, buf.end(), pattern.begin(), pattern.end())) != buf.end();
                     ++it) {
                    if (hits++ < max_hits)
                        std::printf("found %08x\n", base + static_cast<u32>(it - buf.begin()));
                }
            }
            if (cmd == "find")
                std::printf("find: %u hits\n", hits);
#ifdef LEAF_DEBUG_HOOKS
        } else if (cmd == "watch" || cmd == "bp") {
            std::string a;
            u32 n = 0, max_hits = 200;
            ss >> a;
            auto& hooks = Leaf::Debug::GetHooks();
            static u32 hit_count, hit_max;
            hit_count = 0;
            const auto print_regs = [](const Leaf::Debug::Regs& r) {
                for (int i = 0; i < 16; i++)
                    std::printf("%sr%d=%08x", i % 8 ? " " : "\n   ", i, r[i]);
                std::printf("\n");
            };
            if (cmd == "watch") {
                ss >> std::hex >> n >> std::dec >> max_hits;
                hit_max = max_hits;
                hooks.watch_begin = std::stoul(a, nullptr, 16);
                hooks.watch_end = hooks.watch_begin + n;
                hooks.on_write = [print_regs](VAddr addr, u32 size, u64 value,
                                              const Leaf::Debug::Regs& r) {
                    if (hit_count++ >= hit_max)
                        return;
                    std::printf("write %08x size %u = %llx pc=%08x lr=%08x frame %llu", addr, size,
                                static_cast<unsigned long long>(value), r[15], r[14],
                                static_cast<unsigned long long>(g_frames));
                    print_regs(r);
                };
            } else {
                ss >> max_hits;
                hit_max = max_hits;
                std::stringstream ls(a);
                std::string item;
                while (std::getline(ls, item, ','))
                    hooks.exec.insert(std::stoul(item, nullptr, 16));
                hooks.on_exec = [print_regs](const Leaf::Debug::Regs& r) {
                    if (hit_count++ >= hit_max)
                        return;
                    std::printf("exec pc=%08x lr=%08x frame %llu", r[15], r[14],
                                static_cast<unsigned long long>(g_frames));
                    print_regs(r);
                };
            }
            hooks.active = true;
        } else if (cmd == "rwatch") {
            std::string a;
            u32 n = 0;
            static u32 rhits, rmax;
            rhits = 0;
            rmax = 200;
            ss >> a >> std::hex >> n >> std::dec >> rmax;
            auto& hooks = Leaf::Debug::GetHooks();
            hooks.read_begin = std::stoul(a, nullptr, 16);
            hooks.read_end = hooks.read_begin + n;
            hooks.on_read = [](VAddr addr, u32 size, const Leaf::Debug::Regs& r) {
                if (rhits++ < rmax)
                    std::printf("read %08x size %u pc=%08x lr=%08x frame %llu\n", addr, size,
                                r[15], r[14], static_cast<unsigned long long>(g_frames));
            };
            hooks.active = true;
        } else if (cmd == "unwatch") {
            Leaf::Debug::GetHooks().watch_end = 0;
            Leaf::Debug::GetHooks().read_end = 0;
        } else if (cmd == "unbp") {
            Leaf::Debug::GetHooks().exec.clear();
#endif
        } else if (cmd == "frames") {
            std::printf("frame %llu\n", static_cast<unsigned long long>(g_frames));
        } else {
            std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
        }
        std::fflush(stdout);
    }

    system.Shutdown();
    Common::Log::Stop();
    return 0;
}
