// Copyright 2026 Poly
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <optional>
#include "common/logging/log.h"
#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/hle/kernel/process.h"
#include "core/leaf/code_search.h"
#include "core/leaf/leaf.h"
#include "core/leaf/tool_ring.h"
#include "core/memory.h"

namespace Leaf::ToolRing {

namespace {

using namespace CodeSearch;
using Clock = std::chrono::steady_clock;

struct Item {
    u16 id;
    u16 flags;
};

constexpr u16 EmptyItem = 0x7FFE;
constexpr int PocketSlots = 16;
constexpr u32 LockOffset = 0x40; // one lock byte per slot after the 16 items
constexpr u32 HeldOffset = 0x26; // tool in hand, in the player's save data
constexpr u32 StateOffset = 0x1A9; // player actor: current state
constexpr u8 StateStanding = 0x06;

constexpr std::array<u8, 7> Kinds{Axe, Net, Rod, Shovel, Slingshot, WateringCan, Megaphone};

/// Icon (ItemWin/item_icon.arc icn_<n>) per kind and variant (rental, regular, silver, gold),
/// as the pockets screen shows them; the same in both releases.
u16 IconFor(u8 kind, u8 variant) {
    static constexpr std::array<std::array<u16, 4>, 6> icons{{
        {319, 319, 350, 322}, // axe
        {325, 325, 353, 326}, // net
        {323, 323, 352, 324}, // fishing rod
        {317, 317, 351, 318}, // shovel
        {363, 363, 365, 364}, // slingshot
        {327, 327, 354, 328}, // watering can
    }};
    if (kind == Megaphone)
        return 482;
    return icons[kind - Axe][variant & 3];
}

// Stub, at `cave`, entered instead of the player update:
//   push {r0-r3, r12, lr}; svc #RingSvc          ; r0 = actor -> r0 = item to equip or 0
//   cmp r0, #0; beq 1f
//   mov r1, #0; mov r2, #0; bl Equip; svc #RingSvc ; r0 = result
//   1: pop {r0-r3, r12, lr}; <first instruction of the update>; b update + 4
//   item buffer
constexpr u32 SvcPre = 0x04;
constexpr u32 SvcPost = 0x1C;
constexpr u32 Buffer = 0x30;
constexpr u32 CaveSize = 0x40;

struct Located {
    bool ok = false;
    VAddr update = 0;
    u32 update_first = 0;
    VAddr equip = 0;
    VAddr game = 0;          ///< [game + player_offset] = the player's save data
    u32 player_offset = 0;
    u32 pockets_offset = 0;  ///< from the player's save data
    VAddr category_table = 0;
    u32 category_count = 0;
    VAddr cave = 0;
};

Located g;

/// Snapshot of the player, taken on the emulation thread at every player update.
struct Snapshot {
    bool standing = false;
    Clock::time_point when{};
    std::array<Item, PocketSlots> pockets{};
    std::array<u8, PocketSlots> locks{};
    Item held{EmptyItem, 0};
};

std::mutex g_mutex;
Snapshot g_snapshot;
std::optional<int> g_request;   ///< slot to take out, -1 = put away
std::optional<int> g_in_flight; ///< pocket slot that receives the tool put away
VAddr g_actor = 0;
Clock::time_point g_other_actor{};
std::array<std::pair<u16, u16>, 0x20> g_categories{}; ///< first/last id of categories 0..0x1F

bool LocateImpl(Core::System& system, Kernel::Process& process) {
    const CodeView code(system, process, "tool ring");
    Located found;

    // The request behind "equip this item": stores the item and switches the player actor to
    // its tool-switch state.
    //   push {r4-r6, lr}; mov r5, r1; mov r4, r2; mov r6, r0; bl Request; ldr r1, [r5]
    //   mov r2, r0; mov r3, #0; str r1, [r0, #item]!; mov r1, #state; strb r4, [r0, #4]
    //   mov r0, r6; pop {r4-r6, lr}; b ChangeState
    const auto core = code.Unique({{0xE92D4070},
                                   {0xE1A05001},
                                   {0xE1A04002},
                                   {0xE1A06000},
                                   {0xEB000000, BranchMask},
                                   {0xE5951000},
                                   {0xE1A02000},
                                   {0xE3A03000},
                                   {0xE5A01000, 0xFFFFFF00},
                                   {0xE3A01000, 0xFFFFFF00},
                                   {0xE5C04004},
                                   {0xE1A00006},
                                   {0xE8BD4070},
                                   {0xEA000000, BranchMask}},
                                  "equip request");
    if (!core)
        return false;
    // Its wrapper for the local player (what the pockets screen calls):
    //   push {r4-r6, lr}; mov r4, r0; mov r5, r1; mov r0, #4; mov r1, #0; bl GetPlayer;
    //   cmp r0, #0; beq out; ldrb r1, [r0, #busy]; ... ; b core
    for (const VAddr hit : FindAll(code.words, code.base,
                                   {{0xE92D4070},
                                    {0xE1A04000},
                                    {0xE1A05001},
                                    {0xE3A00004},
                                    {0xE3A01000},
                                    {0xEB000000, BranchMask},
                                    {0xE3500000},
                                    {0x0A000000, BranchMask},
                                    {0xE5D01000, OffsetMask}})) {
        for (VAddr a = hit + 36; a < hit + 72; a += 4) {
            const u32 ins = code.At(a);
            if ((ins & BranchMask) == 0xEA000000 && BranchTarget(a, ins) == *core) {
                if (found.equip != 0)
                    return false;
                found.equip = hit;
            }
        }
    }
    if (found.equip == 0) {
        LOG_WARNING(Core, "Leaf: tool ring: equip routine not found");
        return false;
    }

    // The player actor's per-frame update (Welcome amiibo), or the part of it that runs the
    // current state (original release), entered with the actor in r0.
    std::optional<VAddr> update;
    for (const Signature& sig : {Signature{{0xE92D41F0},
                                           {0xE1A04000},
                                           {0xE2806C01},
                                           {0xE5D00171},
                                           {0xE2866071},
                                           {0xE1A00D00}},
                                 Signature{{0xE92D4070},
                                           {0xE1A04000},
                                           {0xED2D8B02},
                                           {0xE5D001A9},
                                           {0xE35000E0}}}) {
        const auto hits = FindAll(code.words, code.base, sig);
        if (hits.size() == 1)
            update = hits[0];
    }
    if (!update) {
        LOG_WARNING(Core, "Leaf: tool ring: player update not found");
        return false;
    }
    found.update = *update;
    found.update_first = code.At(*update);

    // The player's pockets, as the "room in the pockets?" checks find them:
    //   ldr r0, =game; ldr r0, [r0, #player]; add r0, #0x6000; add r0, #pockets; bl ...
    const auto gates = FindAll(code.words, code.base,
                               {{0xE59F0000, OffsetMask},
                                {0xE5900000, OffsetMask},
                                {0xE2800A06},
                                {0xE2800E00, 0xFFFFFF00},
                                {0xEB000000, BranchMask},
                                {0xE1B00FA0}});
    if (gates.empty())
        return false;
    const auto game_lit = LiteralAddress(gates[0], code.At(gates[0]));
    if (!game_lit)
        return false;
    found.game = ReadLiteral(system, process, *game_lit);
    found.player_offset = code.At(gates[0] + 4) & 0xFFF;
    found.pockets_offset = 0x6000 + Immediate(code.At(gates[0] + 12));

    // Category table: {u16 first id, u16 last id, u32 count} per category.
    const auto categories = code.Unique(
        {{0xE3500000, 0xFFFFFF00}, {0x359F1000, OffsetMask}, {0x30800080}, {0x30810100}},
        "item categories");
    if (!categories)
        return false;
    found.category_count = code.At(*categories) & 0xFF;
    const auto table_lit = LiteralAddress(*categories + 4, code.At(*categories + 4));
    if (!table_lit || found.category_count <= Megaphone)
        return false;
    found.category_table = ReadLiteral(system, process, *table_lit);

    // Room for the stub: zero padding at the end of the code segment, before the auto-stack
    // stubs (the last 0x80 bytes).
    found.cave = code.End() - 0x80 - CaveSize;
    for (VAddr a = found.cave; a < code.End(); a += 4) {
        if (a < code.End() - 0x80 && code.At(a) != 0)
            return false;
    }

    for (u8 c = 0; c < g_categories.size() && c < found.category_count; c++) {
        g_categories[c] = {system.Memory().Read16(process, found.category_table + c * 12),
                           system.Memory().Read16(process, found.category_table + c * 12 + 2)};
    }

    found.ok = true;
    g = found;
    LOG_INFO(Core,
             "Leaf: tool ring available (update {:08X}, equip {:08X}, player [{:08X}+{:X}], "
             "pockets +{:X}, stub {:08X})",
             g.update, g.equip, g.game, g.player_offset, g.pockets_offset, g.cave);
    return true;
}

/// Kind and variant of a tool the ring offers.
std::optional<std::pair<u8, u8>> Classify(Item item) {
    const u16 id = item.id & 0x7FFF;
    for (const u8 kind : Kinds) {
        const auto [first, last] = g_categories[kind];
        if (id >= first && id <= last)
            return std::pair<u8, u8>{kind, static_cast<u8>(id - first)};
    }
    return std::nullopt;
}

bool Unlocked(u8 lock) {
    return lock == 0 || lock == 2; // 2 is set by the game for whole item kinds
}

std::array<u32, CaveSize / 4> StubWords() {
    const VAddr c = g.cave;
    std::array<u32, CaveSize / 4> words{};
    const std::array<u32, 12> code{
        0xE92D500F,                               // push {r0-r3, r12, lr}
        0xEF000000 | RingSvc,                     // svc
        0xE3500000,                               // cmp r0, #0
        0x0A000004,                               // beq 1f
        0xE3A01000,                               // mov r1, #0
        0xE3A02000,                               // mov r2, #0
        Branch(0xEB000000, c + 0x18, g.equip),    // bl Equip
        0xEF000000 | RingSvc,                     // svc
        0xE1A00000,                               // nop
        0xE8BD500F,                               // 1: pop {r0-r3, r12, lr}
        g.update_first,                           // the update's own first instruction
        Branch(0xEA000000, c + 0x2C, g.update + 4), // b update + 4
    };
    std::copy(code.begin(), code.end(), words.begin());
    return words;
}

VAddr PlayerData(Memory::MemorySystem& memory) {
    return memory.Read32(g.game + g.player_offset);
}

} // namespace

void Reset() {
    std::scoped_lock lock{g_mutex};
    g = {};
    g_snapshot = {};
    g_request.reset();
    g_in_flight.reset();
    g_actor = 0;
}

void Locate(Core::System& system, Kernel::Process& process) {
    Reset();
    if (!LocateImpl(system, process)) {
        g = {};
        LOG_WARNING(Core, "Leaf: tool ring not available for this game version");
    }
}

bool Available() {
    return g.ok;
}

void Apply(Core::System& system) {
    auto process = system.Kernel().GetCurrentProcess();
    if (!g.ok || !process || !process->codeset || !IsLeafTitle(process->codeset->program_id))
        return;
    auto& memory = system.Memory();
    const auto stub = StubWords();
    for (std::size_t i = 0; i < stub.size(); i++)
        memory.Write32(*process, g.cave + static_cast<VAddr>(i * 4), stub[i]);
    system.InvalidateCacheRange(g.cave, CaveSize);
    const u32 redirect = Branch(0xEA000000, g.update, g.cave);
    if (memory.Read32(*process, g.update) != redirect) {
        memory.Write32(*process, g.update, redirect);
        system.InvalidateCacheRange(g.update, 4);
    }
    std::scoped_lock lock{g_mutex};
    g_request.reset();
    g_in_flight.reset();
    g_snapshot = {};
}

std::vector<Tool> Tools() {
    std::vector<Tool> tools;
    std::scoped_lock lock{g_mutex};
    // A snapshot that old means the player isn't being updated (emulation paused, loading).
    if (!g.ok || !g_snapshot.standing || Clock::now() - g_snapshot.when > std::chrono::seconds(1))
        return tools;
    const auto add = [&](int slot, Item item) {
        const auto kind = Classify(item);
        if (kind && tools.size() < MaxTools)
            tools.push_back({slot, kind->first, kind->second, IconFor(kind->first, kind->second)});
    };
    add(-1, g_snapshot.held);
    // One of each different tool first, then the others, in pocket order.
    std::array<bool, PocketSlots> used{};
    for (int i = 0; i < PocketSlots; i++) {
        const auto kind = Classify(g_snapshot.pockets[i]);
        if (!kind || !Unlocked(g_snapshot.locks[i]))
            continue;
        const bool seen = std::any_of(tools.begin(), tools.end(), [&](const Tool& t) {
            return t.kind == kind->first && t.variant == kind->second;
        });
        if (!seen) {
            add(i, g_snapshot.pockets[i]);
            used[i] = true;
        }
    }
    for (int i = 0; i < PocketSlots; i++) {
        if (!used[i] && Unlocked(g_snapshot.locks[i]))
            add(i, g_snapshot.pockets[i]);
    }
    return tools;
}

void Select(int slot) {
    std::scoped_lock lock{g_mutex};
    if (g.ok && slot >= -1 && slot < PocketSlots)
        g_request = slot;
}

bool HandleSvc(Core::System& system, u32 immediate) {
    if (immediate != RingSvc || !g.ok)
        return false;
    auto& core = system.GetRunningCore();
    // Dynarmic reports the instruction after the SVC, the interpreter the SVC itself.
    const VAddr pc = core.GetPC();
    const bool pre = pc == g.cave + SvcPre || pc == g.cave + SvcPre + 4;
    const bool post = pc == g.cave + SvcPost || pc == g.cave + SvcPost + 4;
    if (!pre && !post)
        return false;

    auto& memory = system.Memory();
    const VAddr player = PlayerData(memory);
    const VAddr pockets = player + g.pockets_offset;
    const VAddr held = player + HeldOffset;
    const auto read = [&](VAddr a) { return Item{memory.Read16(a), memory.Read16(a + 2)}; };
    const auto write = [&](VAddr a, Item item) {
        memory.Write16(a, item.id);
        memory.Write16(a + 2, item.flags);
    };
    std::scoped_lock lock{g_mutex};

    if (post) {
        // r0 = result of the equip routine: only then do the pockets change, like on the
        // pockets screen (the tool put away takes the freed slot).
        if (core.GetReg(0) != 0 && g_in_flight)
            write(pockets + *g_in_flight * 4, read(held));
        g_in_flight.reset();
        return true;
    }

    // Before the player update: refresh the snapshot, then start a pending switch.
    const VAddr actor = core.GetReg(0);
    const auto now = Clock::now();
    if (g_actor != 0 && actor != g_actor)
        g_other_actor = now; // several players (club, dream): the ring stays off
    g_actor = actor;
    Snapshot snap;
    snap.when = now;
    snap.standing = memory.Read8(actor + StateOffset) == StateStanding &&
                    now - g_other_actor > std::chrono::seconds(2);
    for (int i = 0; i < PocketSlots; i++) {
        snap.pockets[i] = read(pockets + i * 4);
        snap.locks[i] = memory.Read8(pockets + LockOffset + i);
    }
    snap.held = read(held);
    g_snapshot = snap;

    core.SetReg(0, 0);
    if (!g_request)
        return true;
    const int slot = *g_request;
    g_request.reset();
    if (!snap.standing)
        return true;
    Item take{EmptyItem, 0};
    int receiving = slot;
    if (slot < 0) {
        if ((snap.held.id & 0x7FFF) == EmptyItem)
            return true;
        receiving = -1;
        for (int i = 0; i < PocketSlots && receiving < 0; i++) {
            if ((snap.pockets[i].id & 0x7FFF) == EmptyItem && Unlocked(snap.locks[i]))
                receiving = i;
        }
        if (receiving < 0)
            return true; // no room: nothing happens, as on the pockets screen
    } else {
        if (!Classify(snap.pockets[slot]) || !Unlocked(snap.locks[slot]))
            return true;
        take = snap.pockets[slot];
    }
    write(g.cave + Buffer, take);
    g_in_flight = receiving;
    core.SetReg(0, g.cave + Buffer);
    return true;
}

} // namespace Leaf::ToolRing
