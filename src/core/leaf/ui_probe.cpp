// Copyright 2026 Poly
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include "common/hash.h"
#include "core/leaf/ui_probe.h"
#include "core/memory.h"

namespace Leaf::UiProbe {

namespace {

struct DrawnTexture {
    PAddr address;
    u32 width;
    u32 height;
    u32 format;
    bool has_bounds = false;
    std::array<float, 4> bounds{}; ///< render target pixels, union of all its draws
};

void MergeBounds(DrawnTexture& t, const std::array<float, 4>* bounds) {
    if (!bounds)
        return;
    if (!t.has_bounds) {
        t.bounds = *bounds;
        t.has_bounds = true;
        return;
    }
    t.bounds[0] = std::min(t.bounds[0], (*bounds)[0]);
    t.bounds[1] = std::min(t.bounds[1], (*bounds)[1]);
    t.bounds[2] = std::max(t.bounds[2], (*bounds)[2]);
    t.bounds[3] = std::max(t.bounds[3], (*bounds)[3]);
}

/// Bottom screen: rendered into a 240x320 portrait target (stored rotated); its x runs along
/// the screen's height (bottom to top) and its y from the screen's right edge to the left.
Common::Rectangle<float> ToBottomScreen(const std::array<float, 4>& b) {
    return {320.0f - b[3], 240.0f - b[2], 320.0f - b[1], 240.0f - b[0]};
}

struct CachedKey {
    u64 sample; ///< hash of the first bytes, to notice when an address gets new contents
    u64 key;
};

// Emu-thread state.
std::vector<DrawnTexture> g_pending_textures; ///< drawn since the last display transfer
std::unordered_map<PAddr, std::size_t> g_pending_index; ///< texture address -> pending entry
std::vector<DrawnTexture> g_frame_textures;   ///< attributed to the bottom screen this frame
std::vector<DrawnTexture> g_top_textures;     ///< attributed to the top screen this frame
std::unordered_map<PAddr, CachedKey> g_key_cache;
u32 g_bottom_transfers = 0; ///< bottom-screen transfers since the last EndFrame

// Shared with the frontend / scene logic.
std::mutex g_mutex;
std::unordered_map<u64, u32> g_known;
std::vector<u32> g_visible;
std::vector<u64> g_last_keys;
std::vector<u64> g_last_top_keys;
std::vector<std::pair<u64, Common::Rectangle<float>>> g_last_bounds;
std::atomic<bool> g_want_bounds{false};

u32 BitsPerPixel(u32 format) {
    switch (format) {
    case 0: // RGBA8
        return 32;
    case 1: // RGB8
        return 24;
    case 2: // RGB5A1
    case 3: // RGB565
    case 4: // RGBA4
    case 5: // IA8
    case 6: // RG8 / HILO8
        return 16;
    case 7: // I8
    case 8: // A8
    case 9: // IA4
        return 8;
    case 10: // I4
    case 11: // A4
    case 12: // ETC1
        return 4;
    case 13: // ETC1A4
        return 8;
    default:
        return 0;
    }
}

} // namespace

void OnDraw(PAddr color_buffer, bool texture_enabled, PAddr texture, u32 width, u32 height,
            u32 format, const std::array<float, 4>* bounds) {
    // The game renders both screens into a shared render target one after the other, so draws
    // are attributed to a screen by the display transfer that follows them.
    if (!texture_enabled)
        return;
    if (const auto it = g_pending_index.find(texture); it != g_pending_index.end()) {
        MergeBounds(g_pending_textures[it->second], bounds);
        return;
    }
    if (g_pending_textures.size() < 256) {
        DrawnTexture t{texture, width, height, format};
        MergeBounds(t, bounds);
        g_pending_index.emplace(texture, g_pending_textures.size());
        g_pending_textures.push_back(t);
    }
}

bool WantBounds() {
    return g_want_bounds.load(std::memory_order_relaxed);
}

void SetWantBounds(bool want) {
    g_want_bounds.store(want, std::memory_order_relaxed);
}

void OnDisplayTransfer(PAddr input, u32 output_width, u32 output_height) {
    // The bottom LCD output is 320 pixels tall (stored rotated, width padded to 256);
    // the top one is 400 (416 padded).
    const bool bottom = output_height == 320 || (output_width == 320 && output_height <= 256);
    auto& target = bottom ? g_frame_textures : g_top_textures;
    if (bottom)
        g_bottom_transfers++;
    for (const auto& t : g_pending_textures) {
        const auto seen = std::find_if(target.begin(), target.end(),
                                       [&](const auto& f) { return f.address == t.address; });
        if (seen != target.end())
            MergeBounds(*seen, t.has_bounds ? &t.bounds : nullptr);
        else if (target.size() < 512)
            target.push_back(t);
    }
    g_pending_textures.clear();
    g_pending_index.clear();
}

u64 TextureKey(const u8* data, std::size_t size, u32 width, u32 height) {
    return Common::ComputeHash64(data, size) ^ (static_cast<u64>(width) << 48) ^
           (static_cast<u64>(height) << 32);
}

static std::vector<u64> KeysOf(Memory::MemorySystem& memory, std::vector<DrawnTexture>& textures) {
    std::vector<u64> keys;
    keys.reserve(textures.size());
    for (const auto& t : textures) {
        const u32 bpp = BitsPerPixel(t.format);
        const std::size_t size = static_cast<std::size_t>(t.width) * t.height * bpp / 8;
        const u8* data = memory.GetPhysicalPointer(t.address);
        if (!data || size == 0)
        {
            keys.push_back(0); // keeps keys aligned with `textures`
            continue;
        }
        const u64 sample = Common::ComputeHash64(data, std::min<std::size_t>(size, 64));
        auto it = g_key_cache.find(t.address);
        if (it == g_key_cache.end() || it->second.sample != sample) {
            it = g_key_cache.insert_or_assign(t.address,
                                              CachedKey{sample, TextureKey(data, size, t.width,
                                                                           t.height)})
                     .first;
        }
        keys.push_back(it->second.key);
    }
    textures.clear();
    return keys;
}

void EndFrame(Memory::MemorySystem& memory) {
    std::vector<u64> top_keys;
    const bool have_top = !g_top_textures.empty();
    if (have_top)
        top_keys = KeysOf(memory, g_top_textures);
    const bool bottom_sent = g_bottom_transfers > 0;
    g_bottom_transfers = 0;
    if (g_frame_textures.empty()) {
        std::scoped_lock lock{g_mutex};
        if (have_top)
            g_last_top_keys = std::move(top_keys);
        if (bottom_sent) {
            // The bottom screen was sent out with nothing drawn on it (e.g. right after the
            // keyboard closes): no UI is up any more.
            g_visible.clear();
            g_last_keys.clear();
            g_last_bounds.clear();
        }
        // Otherwise the bottom screen was not updated this frame; keep its previous result.
        return;
    }
    std::vector<std::pair<bool, std::array<float, 4>>> measured;
    measured.reserve(g_frame_textures.size());
    for (const auto& t : g_frame_textures)
        measured.emplace_back(t.has_bounds, t.bounds);
    auto keys = KeysOf(memory, g_frame_textures);
    std::vector<std::pair<u64, Common::Rectangle<float>>> bounds;
    if (keys.size() == measured.size()) {
        for (std::size_t i = 0; i < keys.size(); i++) {
            if (measured[i].first)
                bounds.emplace_back(keys[i], ToBottomScreen(measured[i].second));
        }
    }

    std::scoped_lock lock{g_mutex};
    g_visible.clear();
    for (const u64 key : keys) {
        if (auto it = g_known.find(key); it != g_known.end())
            g_visible.push_back(it->second);
    }
    g_last_keys = std::move(keys);
    g_last_bounds = std::move(bounds);
    if (have_top)
        g_last_top_keys = std::move(top_keys);
}

void RegisterTexture(u64 key, u32 id) {
    std::scoped_lock lock{g_mutex};
    g_known.emplace(key, id);
}

void ClearRegisteredTextures() {
    std::scoped_lock lock{g_mutex};
    g_known.clear();
}

std::vector<u32> VisibleTextures() {
    std::scoped_lock lock{g_mutex};
    return g_visible;
}

std::vector<u64> LastFrameKeys() {
    std::scoped_lock lock{g_mutex};
    return g_last_keys;
}

std::vector<u64> LastFrameTopKeys() {
    std::scoped_lock lock{g_mutex};
    return g_last_top_keys;
}

std::vector<std::pair<u64, Common::Rectangle<float>>> LastFrameBounds() {
    std::scoped_lock lock{g_mutex};
    return g_last_bounds;
}

} // namespace Leaf::UiProbe
