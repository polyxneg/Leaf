// Copyright 2026 Poly
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <fmt/format.h>
#include "common/hash.h"
#include "core/leaf/leaf.h"
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
// Diagnostics: the last display transfers (for the scene log).
struct TransferNote {
    PAddr input;
    PAddr output;
    bool bottom;
    u32 textured;   ///< textures drawn since the previous transfer
    u32 draws;      ///< draws of any kind since the previous transfer
    PAddr targets[3]; ///< colour buffers drawn into
};
std::array<TransferNote, 8> g_notes{};
std::size_t g_note_next = 0;
u32 g_draws_since_transfer = 0;
std::array<PAddr, 3> g_targets_since_transfer{};
/// What was last sent into each bottom-screen framebuffer. Static screens (e.g. the coupon
/// machine's messages) are redrawn only every few frames: in between the game displays buffers
/// sent from the render target without drawing, which on hardware still holds the last bottom
/// image but here holds the key colour. Those show the last drawn buffer instead.
struct BottomBuffer {
    std::vector<DrawnTexture> textures; ///< empty = sent with nothing drawn on it
    u64 frame = 0;  ///< frame it was (last) sent in; sends in one frame add up
    u64 serial = 0; ///< changes with every frame it is sent in
};
std::unordered_map<PAddr, BottomBuffer> g_bottom_buffers;
u64 g_frame = 1;
u64 g_serial = 0;
PAddr g_shown_bottom = 0;     ///< bottom framebuffer the game currently displays
PAddr g_drawn_bottom = 0;     ///< buffer the bottom screen was last drawn into
u64 g_drawn_frame = 0;        ///< frame of that
/// Frames an undrawn bottom screen may follow a drawn one and still repeat it (static screens
/// redraw every third frame); longer means the screen really is empty now.
constexpr u64 RepeatFrames = 4;
/// Top-screen textures of screens whose bottom screen the game draws once and then only sends
/// out again (see SetHoldTopKeys), and whether one is up.
std::unordered_set<u64> g_hold_top_keys;
bool g_hold = false;
PAddr g_processed_bottom = 0; ///< buffer (and its serial) the current result comes from
u64 g_processed_serial = 0;
PAddr g_top_input = 0;
u32 g_top_input_size = 0;

// Shared with the frontend / scene logic.
std::mutex g_mutex;
std::unordered_map<u64, u32> g_known;
std::vector<u32> g_visible;
std::vector<u64> g_last_keys;
std::vector<u64> g_last_top_keys;
std::vector<std::pair<u64, Common::Rectangle<float>>> g_last_bounds;
std::atomic<bool> g_want_bounds{false};


} // namespace

void OnDraw(PAddr color_buffer, bool texture_enabled, PAddr texture, u32 width, u32 height,
            u32 format, const std::array<float, 4>* bounds) {
    // The game renders both screens into a shared render target one after the other, so draws
    // are attributed to a screen by the display transfer that follows them.
    g_draws_since_transfer++;
    for (auto& t : g_targets_since_transfer) {
        if (t == color_buffer)
            break;
        if (t == 0) {
            t = color_buffer;
            break;
        }
    }
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

void OnBottomSwap(PAddr shown) {
    g_shown_bottom = shown;
}

static std::unordered_set<u64> g_ring_hidden_keys; ///< the clock plate
static std::unordered_map<PAddr, CachedKey> g_skip_cache;

void SetRingHiddenKeys(std::unordered_set<u64> keys) {
    g_ring_hidden_keys = std::move(keys);
    g_skip_cache.clear();
}

bool SkipDraw(const u8* data, PAddr texture, u32 width, u32 height, u32 format) {
    if (!GetConfig().tool_ring_open.load(std::memory_order_relaxed))
        return false;
    // Font sheets are A4 or IA4 (the number fonts too).
    if (format == 11 || format == 9)
        return true;
    const std::size_t size = static_cast<std::size_t>(width) * height * BitsPerPixel(format) / 8;
    if (!data || size == 0 || g_ring_hidden_keys.empty())
        return false;
    const u64 sample = Common::ComputeHash64(data, std::min<std::size_t>(size, 64));
    auto it = g_skip_cache.find(texture);
    if (it == g_skip_cache.end() || it->second.sample != sample) {
        it = g_skip_cache
                 .insert_or_assign(texture,
                                   CachedKey{sample, TextureKey(data, size, width, height)})
                 .first;
    }
    return g_ring_hidden_keys.contains(it->second.key);
}

void SetHoldTopKeys(std::unordered_set<u64> keys) {
    g_hold_top_keys = std::move(keys);
    g_hold = false;
}

bool HoldBottom() {
    return g_hold;
}

PAddr DisplayedBottom(PAddr shown) {
    const auto it = g_bottom_buffers.find(shown);
    if (it == g_bottom_buffers.end() || !it->second.textures.empty() || g_drawn_bottom == 0 ||
        (!g_hold && g_frame - g_drawn_frame > RepeatFrames))
        return shown;
    const auto drawn = g_bottom_buffers.find(g_drawn_bottom);
    if (drawn == g_bottom_buffers.end() || drawn->second.textures.empty())
        return shown; // overwritten since
    return g_drawn_bottom;
}

void OnDisplayTransfer(PAddr input, PAddr output, u32 input_size, u32 output_width,
                       u32 output_height) {
    // The bottom LCD output is 320 pixels tall (stored rotated, width padded to 256);
    // the top one is 400 (416 padded).
    const bool bottom = output_height == 320 || (output_width == 320 && output_height <= 256);
    auto& target = bottom ? g_frame_textures : g_top_textures;
    {
        auto& note = g_notes[g_note_next++ % g_notes.size()];
        note = {input, output, bottom, static_cast<u32>(g_pending_textures.size()),
                g_draws_since_transfer,
                {g_targets_since_transfer[0], g_targets_since_transfer[1],
                 g_targets_since_transfer[2]}};
        g_draws_since_transfer = 0;
        g_targets_since_transfer = {};
    }
    if (bottom) {
        g_bottom_transfers++;
        // Nothing drawn only means an empty screen when the input is the render target the top
        // screen also uses; a bottom screen kept in its own buffer is just sent again unchanged.
        const bool shared = input >= g_top_input && input < g_top_input + g_top_input_size;
        if (g_pending_textures.empty() && !shared) {
            g_pending_index.clear();
            return;
        }
        auto& buffer = g_bottom_buffers[output];
        if (buffer.frame != g_frame) {
            buffer.textures.clear();
            buffer.frame = g_frame;
            buffer.serial = ++g_serial;
        }
        for (const auto& t : g_pending_textures) {
            const auto seen = std::find_if(buffer.textures.begin(), buffer.textures.end(),
                                           [&](const auto& f) { return f.address == t.address; });
            if (seen != buffer.textures.end())
                MergeBounds(*seen, t.has_bounds ? &t.bounds : nullptr);
            else if (buffer.textures.size() < 512)
                buffer.textures.push_back(t);
        }
        if (!buffer.textures.empty()) {
            g_drawn_bottom = output;
            g_drawn_frame = g_frame;
        }
        g_pending_textures.clear();
        g_pending_index.clear();
        return;
    }
    g_top_input = input;
    g_top_input_size = input_size;
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
    struct NextFrame {
        ~NextFrame() {
            g_frame++;
        }
    } next_frame;
    std::vector<u64> top_keys;
    const bool have_top = !g_top_textures.empty();
    if (have_top) {
        top_keys = KeysOf(memory, g_top_textures);
        g_hold = std::any_of(top_keys.begin(), top_keys.end(),
                             [](u64 key) { return g_hold_top_keys.contains(key); });
    }
    g_bottom_transfers = 0;
    // The bottom screen as displayed: the buffer the game shows (the last one sent if the
    // game has not swapped any in yet).
    PAddr shown = DisplayedBottom(g_shown_bottom);
    if (!g_bottom_buffers.contains(shown)) {
        u64 newest = 0;
        for (const auto& [address, buffer] : g_bottom_buffers) {
            if (buffer.serial > newest) {
                newest = buffer.serial;
                shown = address;
            }
        }
    }
    const auto it = g_bottom_buffers.find(shown);
    if (it == g_bottom_buffers.end() ||
        (shown == g_processed_bottom && it->second.serial == g_processed_serial)) {
        // The displayed bottom screen did not change; keep its previous result.
        std::scoped_lock lock{g_mutex};
        if (have_top)
            g_last_top_keys = std::move(top_keys);
        return;
    }
    g_processed_bottom = shown;
    g_processed_serial = it->second.serial;
    g_frame_textures = it->second.textures;
    if (g_frame_textures.empty()) {
        // Shown with nothing drawn on it (e.g. after the keyboard closes): no UI is up.
        std::scoped_lock lock{g_mutex};
        if (have_top)
            g_last_top_keys = std::move(top_keys);
        g_visible.clear();
        g_last_keys.clear();
        g_last_bounds.clear();
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

std::string RecentTransfers() {
    std::string out;
    for (std::size_t i = 0; i < g_notes.size(); i++) {
        const auto& n = g_notes[(g_note_next + i) % g_notes.size()];
        if (n.input == 0)
            continue;
        out += fmt::format(" [{:08x}>{:08x} {} t{} d{} rt{:08x},{:08x}]", n.input, n.output,
                           n.bottom ? "B" : "T", n.textured, n.draws, n.targets[0],
                           n.targets[1]);
    }
    out += fmt::format(" shown {:08x}", g_shown_bottom);
    return out;
}

std::vector<std::pair<u64, Common::Rectangle<float>>> LastFrameBounds() {
    std::scoped_lock lock{g_mutex};
    return g_last_bounds;
}

} // namespace Leaf::UiProbe
