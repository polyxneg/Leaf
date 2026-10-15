// Copyright 2026 Poly
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <algorithm>
#include <cmath>
#include <array>
#include <atomic>
#include <cstring>
#include <vector>
#include "common/logging/log.h"
#include "core/core.h"
#include "core/hle/kernel/process.h"
#include "core/leaf/debug_hooks.h"
#include "core/leaf/leaf.h"
#include "core/leaf/pocket_stacks.h"
#include "core/leaf/ui_scenes.h"
#include "core/memory.h"

namespace Leaf {

namespace {

Config g_config;
std::atomic<bool> g_active{false};

struct CameraParamSite {
    VAddr table = 0;  // read-only default {near, far, fovy, aspect}
    VAddr global = 0; // writable copy made by the game's static initializer
};

std::vector<CameraParamSite> g_camera_sites;

u32 FloatBits(float f) {
    u32 v;
    std::memcpy(&v, &f, sizeof(v));
    return v;
}

/// Finds the field camera's default projection parameters:
///   near = 50.0f, far = 1750.0f, fovy = 22.0f (degrees), aspect = 400/240.
/// The game copies each table into a global during static init, using a pair of adjacent literal
/// pool words {table address, global address}; we find that pair too so running games can be
/// patched.
void LocateCameraParams(Core::System& system, Kernel::Process& process) {
    g_camera_sites.clear();
    auto& memory = system.Memory();
    const auto& ro = process.codeset->RODataSegment();
    const auto& code = process.codeset->CodeSegment();
    const auto& data = process.codeset->DataSegment();

    std::vector<u32> ro_words(ro.size / 4);
    memory.ReadBlock(process, ro.addr, ro_words.data(), ro_words.size() * 4);
    std::vector<u32> code_words(code.size / 4);
    memory.ReadBlock(process, code.addr, code_words.data(), code_words.size() * 4);

    const std::array<u32, 4> signature = {FloatBits(50.0f), FloatBits(1750.0f), FloatBits(22.0f),
                                          FloatBits(NativeAspect)};
    // .bss follows .data but is not part of the CodeSet; 8 MiB comfortably covers it.
    const VAddr data_end = data.addr + data.size + 0x800000;

    for (std::size_t i = 0; i + signature.size() <= ro_words.size(); i++) {
        if (!std::equal(signature.begin(), signature.end(), ro_words.begin() + i))
            continue;
        CameraParamSite site;
        site.table = ro.addr + static_cast<VAddr>(i * 4);
        for (std::size_t j = 0; j + 1 < code_words.size(); j++) {
            const VAddr next = code_words[j + 1];
            if (code_words[j] == site.table && next >= data.addr && next < data_end) {
                site.global = next;
                break;
            }
        }
        LOG_INFO(Core, "Leaf: camera params at {:08X} (global {:08X})", site.table, site.global);
        g_camera_sites.push_back(site);
    }
    if (g_camera_sites.empty())
        LOG_WARNING(Core, "Leaf: camera parameters not found, aspect patch disabled");
}

} // namespace

Config& GetConfig() {
    return g_config;
}

float DisplayAspect(DisplayMode mode, u32 window_width, u32 window_height) {
    switch (mode) {
    case DisplayMode::Wide16x9:
        return 9.0f / 16.0f;
    case DisplayMode::Classic4x3:
        return 3.0f / 4.0f;
    case DisplayMode::Native5x3:
        return 1.0f / NativeAspect;
    case DisplayMode::Fill:
    default:
        if (window_width == 0)
            return 1.0f / NativeAspect;
        return static_cast<float>(window_height) / static_cast<float>(window_width);
    }
}

float CameraAspectFor(DisplayMode mode, u32 window_width, u32 window_height) {
    if (mode == DisplayMode::Native5x3)
        return 0.0f;
    return 1.0f / DisplayAspect(mode, window_width, window_height);
}

bool IsLeafTitle(u64 title_id) {
    switch (title_id & ~0xF00ULL) {          // ignore the region nibble
    case 0x0004000000086000ULL:              // 86200 JP, 86300 US, 86400 EU, 86500 KR
    case 0x0004000000198000ULL:              // Welcome amiibo standalone listings
        return true;
    default:
        return false;
    }
}

bool IsActive() {
    return g_active.load(std::memory_order_relaxed);
}

void OnProgramLoaded(Core::System& system, u64 title_id, Kernel::Process& process) {
    g_camera_sites.clear();
    PocketStacks::Reset();
    g_active = IsLeafTitle(title_id);
    g_config.layout_enabled = g_active.load();
    if (!g_active)
        return;
    LOG_INFO(Core, "Leaf: Animal Crossing: New Leaf detected ({:016X})", title_id);
    LocateCameraParams(system, process);
    ApplyCameraAspect(system);
    PocketStacks::Locate(system, process);
    PocketStacks::Apply(system);
    Ui::Initialize(system.GetAppLoader());
}

void OnStateLoaded(Core::System& system) {
    PocketStacks::Apply(system);
}

void ApplyCameraAspect(Core::System& system) {
    // Looked up on every call: loading a savestate replaces the process object.
    auto process = system.Kernel().GetCurrentProcess();
    if (!process || !process->codeset || !IsLeafTitle(process->codeset->program_id) ||
        g_camera_sites.empty())
        return;
    float aspect = g_config.camera_aspect.load();
    if (aspect <= 0.0f)
        aspect = NativeAspect;
    const u32 bits = FloatBits(aspect);
    auto& memory = system.Memory();
    for (const auto& site : g_camera_sites) {
        memory.Write32(*process, site.table + 12, bits);
        if (site.global)
            memory.Write32(*process, site.global + 12, bits);
    }
}

bool CameraPatchAvailable() {
    return !g_camera_sites.empty();
}

std::pair<float, float> ArtScale() {
    const float aspect = g_config.camera_aspect.load();
    if (aspect <= 0.0f)
        return {1.0f, 1.0f};
    const float stretch = aspect / NativeAspect;
    return stretch >= 1.0f ? std::pair{1.0f / stretch, 1.0f} : std::pair{1.0f, stretch};
}

double PaceScale() {
    constexpr double NativeRefresh = 268111856.0 / 4481136.0; // 59.83 Hz
    const double display = g_config.display_refresh.load();
    if (!g_active || display <= 0.0)
        return 1.0;
    const double frames_per_refresh = std::max(1.0, std::round(display / NativeRefresh));
    const double scale = display / frames_per_refresh / NativeRefresh;
    return std::abs(scale - 1.0) <= 0.01 ? scale : 1.0;
}

const Common::Rectangle<float>* TopScreenCrop() {
    static constexpr Common::Rectangle<float> crop{0.0f, 1.0f / 240.0f, 1.0f, 1.0f};
    return g_active && g_config.layout_enabled.load() ? &crop : nullptr;
}

#ifdef LEAF_DEBUG_HOOKS
Debug::Hooks& Debug::GetHooks() {
    static Hooks hooks;
    return hooks;
}
#endif

} // namespace Leaf
