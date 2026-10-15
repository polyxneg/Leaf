// Copyright 2026 Poly
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <algorithm>
#include <array>
#include <optional>
#include <utility>
#include <vector>
#include "common/logging/log.h"
#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/hle/kernel/process.h"
#include "core/leaf/leaf.h"
#include "core/leaf/pocket_stacks.h"
#include "core/memory.h"

namespace Leaf::PocketStacks {

namespace {

constexpr u32 BranchMask = 0xFF000000;
constexpr u32 OffsetMask = 0xFFFFF000; // ldr/str immediate offset, add immediate
constexpr u32 Exact = 0xFFFFFFFF;

struct Word {
    u32 value;
    u32 mask = Exact;
};
using Signature = std::vector<Word>;

/// Pockets item: id (bit 15 is a flag) and a flag word whose low 14 bits hold "count - 1" for
/// stackable items.
struct Item {
    u16 id;
    u16 flags;
};

constexpr u16 EmptyItem = 0x7FFE;
constexpr int PocketSlots = 16;
constexpr u32 LockOffset = 0x40;   // one lock byte per slot after the 16 items
constexpr u32 NotifyOffset = 0x50; // GiveItem passes pockets + 0x50 to its notify call

// Item categories (the same in the original release and in Welcome amiibo).
constexpr u8 CatBells = 0x02;
constexpr u8 CatStationery = 0x29;
constexpr u8 CatFruit = 0x33;
constexpr u8 CatPerfectFruit = 0x34;
constexpr u8 CatTurnips = 0x39;
constexpr u8 CatBasket = 0x75;
constexpr u8 CatPerfectBasket = 0x76;
constexpr u8 BasketOffset = CatBasket - CatFruit;
constexpr u8 NoFruitMergeScene = 0x77; // the game itself refuses to combine fruit here

constexpr int FruitCap = 9;
constexpr int StationeryCap = 4;
constexpr int BellsHundredsCap = 10; // ten 100-bell bags make a 1,000-bell bag
constexpr int BellsThousandsCap = 99;

/// Where a full-pocket check finds the item it is asked about.
struct GateSite {
    VAddr addr = 0;       ///< the `bl FindFirstFree` instruction
    u32 original = 0;     ///< instruction word before patching
    bool on_stack = false;
    u32 reg_or_offset = 0; ///< register number, or offset from sp
};

struct Located {
    bool ok = false;
    VAddr cave = 0;
    VAddr give_item = 0, notify = 0, find_first_free = 0;
    std::vector<std::pair<VAddr, u32>> pickup_sites; ///< `bl GiveItem` + original word
    std::vector<GateSite> gate_sites;
    VAddr param_holder = 0; ///< [holder + 4] = item parameter table
    u32 param_limit = 0;
    u32 param_size = 0;
    VAddr category_table = 0;
    u32 category_count = 0;
    VAddr scene = 0;
};

Located g;

std::vector<VAddr> FindAll(const std::vector<u32>& code, VAddr base, const Signature& sig) {
    std::vector<VAddr> hits;
    for (std::size_t i = 0; i + sig.size() <= code.size(); i++) {
        bool match = true;
        for (std::size_t k = 0; k < sig.size() && match; k++)
            match = (code[i + k] & sig[k].mask) == (sig[k].value & sig[k].mask);
        if (match)
            hits.push_back(base + static_cast<VAddr>(i * 4));
    }
    return hits;
}

VAddr BranchTarget(VAddr at, u32 ins) {
    s32 offset = static_cast<s32>(ins << 8) >> 8;
    return at + 8 + static_cast<VAddr>(offset * 4);
}

u32 Branch(u32 cond_op, VAddr at, VAddr target) {
    return cond_op | (((target - (at + 8)) >> 2) & 0x00FFFFFF);
}

/// Literal loaded by `ldr rX, [pc, #imm]` at `at`.
std::optional<VAddr> LiteralAddress(VAddr at, u32 ins) {
    if ((ins & 0x0F7F0000) != 0x051F0000)
        return std::nullopt;
    const u32 imm = ins & 0xFFF;
    return (ins & (1u << 23)) ? at + 8 + imm : at + 8 - imm;
}

class CodeView {
public:
    CodeView(Core::System& system, Kernel::Process& process) {
        const auto& seg = process.codeset->CodeSegment();
        base = seg.addr;
        words.resize(seg.size / 4);
        system.Memory().ReadBlock(process, base, words.data(), words.size() * 4);
    }
    bool Contains(VAddr addr) const {
        return addr >= base && addr < base + words.size() * 4;
    }
    u32 At(VAddr addr) const {
        return Contains(addr) ? words[(addr - base) / 4] : 0;
    }
    std::optional<VAddr> Unique(const Signature& sig, const char* what) const {
        const auto hits = FindAll(words, base, sig);
        if (hits.size() != 1) {
            LOG_WARNING(Core, "Leaf: auto-stack: {} found {} times", what, hits.size());
            return std::nullopt;
        }
        return hits[0];
    }

    VAddr base = 0;
    std::vector<u32> words;
};

/// Value of a literal that may live in .rodata (outside the code view).
u32 ReadLiteral(Core::System& system, Kernel::Process& process, VAddr addr) {
    return system.Memory().Read32(process, addr);
}

bool LocateImpl(Core::System& system, Kernel::Process& process) {
    const CodeView code(system, process);
    Located found;

    // GiveItem(pockets, item, lock, notify): copies into the first free slot, then notifies.
    const auto give = code.Unique(
        {{0xE92D40F8}, {0xE1A04000}, {0xE1A07001}, {0xE1A06002}, {0xE1A05003}, {0xE1A0000D},
         {0xEB000000, BranchMask}, {0xE1A0300D}, {0xE1A02006}, {0xE1A01007}, {0xE1A00004},
         {0xEB000000, BranchMask}, {0xE3500000}},
        "GiveItem");
    if (!give)
        return false;
    found.give_item = *give;
    if (code.At(*give + 0x58) != 0xE2840050 || (code.At(*give + 0x5C) & BranchMask) != 0xEB000000)
        return false;
    found.notify = BranchTarget(*give + 0x5C, code.At(*give + 0x5C));

    // The pickup handler: GiveItem(pockets, item, 0, notify).
    for (VAddr a = code.base + 0x10; code.Contains(a); a += 4) {
        const u32 ins = code.At(a);
        if ((ins & BranchMask) == 0xEB000000 && BranchTarget(a, ins) == found.give_item &&
            code.At(a - 4) == 0xE1A00005 && code.At(a - 8) == 0xE1A01004 &&
            code.At(a - 12) == 0xE3A02000 && code.At(a - 16) == 0xE2203001)
            found.pickup_sites.emplace_back(a, ins);
    }
    if (found.pickup_sites.size() != 1) {
        LOG_WARNING(Core, "Leaf: auto-stack: {} pickup sites", found.pickup_sites.size());
        return false;
    }

    // "Is there a free slot for the item under the player?" checks:
    //   ldr r0, =game; ldr r0, [r0, #player]; add r0, #0x6000; add r0, #pockets;
    //   bl FindFirstFree; lsrs r0, r0, #31
    const auto gates = FindAll(code.words, code.base,
                                {{0xE59F0000, OffsetMask},
                                 {0xE5900000, OffsetMask},
                                 {0xE2800A06},
                                 {0xE2800E00, 0xFFFFFF00},
                                 {0xEB000000, BranchMask},
                                 {0xE1B00FA0}});
    for (const VAddr hit : gates) {
        GateSite site;
        site.addr = hit + 16;
        site.original = code.At(site.addr);
        const VAddr target = BranchTarget(site.addr, site.original);
        if (found.find_first_free && target != found.find_first_free)
            return false;
        found.find_first_free = target;
        // The item was checked just before: `mov r0, rN` or `add r0, sp, #imm`, then a bl.
        bool decoded = false;
        for (VAddr a = hit - 4; a + 4 * 12 >= hit && !decoded; a -= 4) {
            if ((code.At(a) & BranchMask) != 0xEB000000)
                continue;
            const u32 set = code.At(a - 4);
            if ((set & 0xFFFFFFF0) == 0xE1A00000) {
                site.on_stack = false;
                site.reg_or_offset = set & 0xF;
                decoded = true;
            } else if ((set & 0xFFFFFF00) == 0xE28D0000) {
                site.on_stack = true;
                site.reg_or_offset = set & 0xFF;
                decoded = true;
            } else {
                break;
            }
        }
        if (!decoded)
            return false;
        found.gate_sites.push_back(site);
    }
    if (found.gate_sites.empty() || found.gate_sites.size() > 4) {
        LOG_WARNING(Core, "Leaf: auto-stack: {} pocket checks", found.gate_sites.size());
        return false;
    }

    // Item parameters: index = (id & 0x7FFF) - 0x2000, table = [holder + 4], entries of
    // 0x22 (original) or 0x1E (Welcome amiibo) bytes, category at +0xA.
    const auto param = code.Unique({{0xE92D4010},
                                    {0xEB000000, BranchMask},
                                    {0xE3500000},
                                    {0xBA000000, BranchMask},
                                    {0xE59F1000, OffsetMask},
                                    {0xE5911004},
                                    {0xE3510000},
                                    {0x159F2000, OffsetMask},
                                    {0x11520000},
                                    {0x80800200, 0xFF1FFFFF}, // addhi or rsbhi (entry size)
                                    {0x80810080},
                                    {0x8A000000, BranchMask},
                                    {0xE3A00000},
                                    {0xE8BD8010}},
                                   "item parameters");
    if (!param)
        return false;
    const VAddr index_fn = BranchTarget(*param + 4, code.At(*param + 4));
    if (code.At(index_fn) != 0xE1D000B0 || code.At(index_fn + 8) != 0xE3C00902 ||
        code.At(index_fn + 12) != 0xE2402A02)
        return false;
    const auto holder_lit = LiteralAddress(*param + 16, code.At(*param + 16));
    const auto limit_lit = LiteralAddress(*param + 28, code.At(*param + 28));
    if (!holder_lit || !limit_lit)
        return false;
    found.param_holder = ReadLiteral(system, process, *holder_lit);
    found.param_limit = ReadLiteral(system, process, *limit_lit);
    switch (code.At(*param + 36)) {
    case 0x80800200: // add r0, r0, r0, lsl #4
        found.param_size = 17 * 2;
        break;
    case 0x80600200: // rsb r0, r0, r0, lsl #4
        found.param_size = 15 * 2;
        break;
    default:
        return false;
    }
    if (code.At(*param + 40) != 0x80810080)
        return false;

    // Category table: {u16 first id, u16 last id, u32 count} per category.
    const auto categories = code.Unique(
        {{0xE3500000, 0xFFFFFF00}, {0x359F1000, OffsetMask}, {0x30800080}, {0x30810100}},
        "item categories");
    if (!categories)
        return false;
    found.category_count = code.At(*categories) & 0xFF;
    const auto table_lit = LiteralAddress(*categories + 4, code.At(*categories + 4));
    if (!table_lit)
        return false;
    found.category_table = ReadLiteral(system, process, *table_lit);

    // The game's own "can these fruit be combined?" check refuses one scene; find its getter.
    const auto fruit_check =
        code.Unique({{0xE92D4070}, {0xE24DD010}, {0xE1A04000}, {0xE1A06001}, {0xE1A05002},
                     {0xEB000000, BranchMask}, {0xE3500077}},
                    "fruit check");
    if (!fruit_check)
        return false;
    const VAddr scene_fn = BranchTarget(*fruit_check + 20, code.At(*fruit_check + 20));
    const auto scene_lit = LiteralAddress(scene_fn, code.At(scene_fn));
    if (!scene_lit || (code.At(scene_fn + 4) & 0xFFFFF000) != 0xE5D00000)
        return false;
    found.scene = ReadLiteral(system, process, *scene_lit) + (code.At(scene_fn + 4) & 0xFFF);

    // Room for the stubs: the zero padding at the end of the code segment.
    const VAddr end = code.base + static_cast<VAddr>(code.words.size() * 4);
    found.cave = end - 0x80;
    for (VAddr a = found.cave; a < end; a += 4) {
        if (code.At(a) != 0)
            return false;
    }

    found.ok = true;
    g = found;
    LOG_INFO(Core,
             "Leaf: auto-stack available (GiveItem {:08X}, pickup {:08X}, {} pocket checks, "
             "items {:08X}/{:X}x{:X}, categories {:08X}, stubs {:08X})",
             g.give_item, g.pickup_sites[0].first, g.gate_sites.size(), g.param_holder,
             g.param_limit, g.param_size, g.category_table, g.cave);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Item rules, mirroring the game's drag-and-drop merge in the pockets.

enum class Type { None, Fruit, Turnips, BellsHundreds, BellsThousands, Stationery };

struct Stack {
    Type type = Type::None;
    u16 key = 0; ///< fruit kind (loose fruit id) or stationery design
    int amount = 0;
    int cap = 0;
};

class Rules {
public:
    Rules(Memory::MemorySystem& memory_) : memory(memory_) {}

    std::optional<u8> Category(u16 id) const {
        const u32 index = static_cast<u32>(id & 0x7FFF) - 0x2000;
        if (index >= g.param_limit)
            return std::nullopt;
        const VAddr table = memory.Read32(g.param_holder + 4);
        if (table == 0)
            return std::nullopt;
        const u8 category = memory.Read8(table + index * g.param_size + 0xA);
        if (category >= g.category_count)
            return std::nullopt;
        return category;
    }
    u16 First(u8 category) const {
        return memory.Read16(g.category_table + category * 12);
    }
    u32 Count(u8 category) const {
        return memory.Read32(g.category_table + category * 12 + 4);
    }

    Stack Classify(Item item) const {
        Stack s;
        if ((item.id & 0x8000) || (item.flags & 0xC000))
            return s;
        const auto category = Category(item.id);
        if (!category)
            return s;
        const int count = (item.flags & 0x3FFF) + 1;
        switch (*category) {
        case CatFruit:
        case CatPerfectFruit:
            if (item.flags || FruitBlocked())
                return s;
            return {Type::Fruit, item.id, 1, FruitCap};
        case CatBasket:
        case CatPerfectBasket: {
            if (count < 2 || count > FruitCap || FruitBlocked())
                return s;
            const u8 fruit = *category - BasketOffset;
            return {Type::Fruit, static_cast<u16>(First(fruit) + (item.id - First(*category))),
                    count, FruitCap};
        }
        case CatTurnips: {
            const int bunches = static_cast<int>(Count(CatTurnips));
            const int amount = item.id - First(CatTurnips) + 1;
            if (item.flags || amount < 1 || amount > bunches)
                return s;
            return {Type::Turnips, 0, amount, bunches};
        }
        case CatBells: {
            const int first = First(CatBells);
            if (item.flags)
                return s;
            if (item.id <= first + 8)
                return {Type::BellsHundreds, 0, item.id - first + 1, BellsHundredsCap};
            return {Type::BellsThousands, 0, item.id - (first + 8), BellsThousandsCap};
        }
        case CatStationery:
            if (count > StationeryCap)
                return s;
            return {Type::Stationery, item.id, count, StationeryCap};
        default:
            return s;
        }
    }

    Item Encode(const Stack& s, int amount) const {
        switch (s.type) {
        case Type::Fruit: {
            if (amount == 1)
                return {s.key, 0};
            const u8 fruit = *Category(s.key);
            const u8 basket = fruit + BasketOffset;
            return {static_cast<u16>(First(basket) + (s.key - First(fruit))),
                    static_cast<u16>(amount - 1)};
        }
        case Type::Turnips:
            return {static_cast<u16>(First(CatTurnips) + amount - 1), 0};
        case Type::BellsHundreds: // 10 x 100 bells is the first thousands bag
            return {static_cast<u16>(First(CatBells) + amount - 1), 0};
        case Type::BellsThousands:
            return {static_cast<u16>(First(CatBells) + 8 + amount), 0};
        case Type::Stationery:
            return {s.key, static_cast<u16>(amount - 1)};
        default:
            return {EmptyItem, 0};
        }
    }

    Item Read(VAddr addr) const {
        return {memory.Read16(addr), memory.Read16(addr + 2)};
    }

    bool FruitBlocked() const {
        return memory.Read8(g.scene) == NoFruitMergeScene;
    }

private:
    Memory::MemorySystem& memory;
};

struct Target {
    int slot;
    Stack stack;
};

/// Stacks in the pockets that `picked` could go into, in slot order.
std::vector<Target> MatchingStacks(const Rules& rules, Memory::MemorySystem& memory,
                                   VAddr pockets, const Stack& picked) {
    std::vector<Target> targets;
    for (int i = 0; i < PocketSlots; i++) {
        if (memory.Read8(pockets + LockOffset + i) != 0)
            continue;
        const Stack s = rules.Classify(rules.Read(pockets + i * 4));
        if (s.type == picked.type && s.key == picked.key && s.amount < s.cap)
            targets.push_back({i, s});
    }
    return targets;
}

int Room(const std::vector<Target>& targets) {
    int room = 0;
    for (const auto& t : targets)
        room += t.stack.cap - t.stack.amount;
    return room;
}

/// Puts the item at `item_addr` into existing stacks if they can take all of it.
bool TryStack(Memory::MemorySystem& memory, VAddr pockets, VAddr item_addr) {
    const Rules rules(memory);
    const Stack picked = rules.Classify(rules.Read(item_addr));
    if (picked.type == Type::None)
        return false;
    const auto targets = MatchingStacks(rules, memory, pockets, picked);
    if (targets.empty() || Room(targets) < picked.amount)
        return false; // the game puts it into a free slot (or refuses), as without Leaf
    int left = picked.amount;
    for (const auto& t : targets) {
        if (left == 0)
            break;
        const int add = std::min(left, t.stack.cap - t.stack.amount);
        const Item merged = rules.Encode(t.stack, t.stack.amount + add);
        memory.Write16(pockets + t.slot * 4, merged.id);
        memory.Write16(pockets + t.slot * 4 + 2, merged.flags);
        left -= add;
    }
    return true;
}

/// Slot of a stack that can take the item at `item_addr`, for a pocket without free slots.
std::optional<int> StackSlotFor(Memory::MemorySystem& memory, VAddr pockets, VAddr item_addr) {
    for (int i = 0; i < PocketSlots; i++) {
        if ((memory.Read16(pockets + i * 4) & 0x7FFF) == EmptyItem)
            return std::nullopt; // a free slot: the game's own answer is fine
    }
    const Rules rules(memory);
    const Stack picked = rules.Classify(rules.Read(item_addr));
    if (picked.type == Type::None)
        return std::nullopt;
    const auto targets = MatchingStacks(rules, memory, pockets, picked);
    if (targets.empty() || Room(targets) < picked.amount)
        return std::nullopt;
    return targets[0].slot;
}

// ---------------------------------------------------------------------------------------------
// Stubs. r12 tells the stub what the emulator did: 0 = continue into the game's function.
//
// cave + 0x00 (replaces `bl GiveItem` in the pickup handler):
//   svc #LeafSvc; cmp r12, #0; beq GiveItem; cmp r12, #2; moveq r0, #1; bxeq lr
//   push {r4, lr}; bl Notify; mov r0, #1; pop {r4, pc}      ; r12 == 1: notify like GiveItem
// cave + 0x40 (replaces `bl FindFirstFree` in the pocket checks):
//   svc #LeafSvc; cmp r12, #0; beq FindFirstFree; bx lr      ; r0 = slot of a stack

constexpr u32 StubB = 0x40;

std::array<u32, 10> StubAWords() {
    const VAddr c = g.cave;
    return {0xEF000000 | LeafSvc,
            0xE35C0000,
            Branch(0x0A000000, c + 8, g.give_item),
            0xE35C0002,
            0x03A00001,
            0x012FFF1E,
            0xE92D4010,
            Branch(0xEB000000, c + 28, g.notify),
            0xE3A00001,
            0xE8BD8010};
}

std::array<u32, 4> StubBWords() {
    const VAddr c = g.cave + StubB;
    return {0xEF000000 | LeafSvc, 0xE35C0000, Branch(0x0A000000, c + 8, g.find_first_free),
            0xE12FFF1E};
}

} // namespace

void Reset() {
    g = {};
}

void Locate(Core::System& system, Kernel::Process& process) {
    g = {};
    if (!LocateImpl(system, process)) {
        g = {};
        LOG_WARNING(Core, "Leaf: auto-stack not available for this game version");
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
    const bool enabled = GetConfig().auto_stack.load();

    // The stubs are inert until a call site points at them, so they are always installed.
    const auto a = StubAWords();
    const auto b = StubBWords();
    for (std::size_t i = 0; i < a.size(); i++)
        memory.Write32(*process, g.cave + static_cast<VAddr>(i * 4), a[i]);
    for (std::size_t i = 0; i < b.size(); i++)
        memory.Write32(*process, g.cave + StubB + static_cast<VAddr>(i * 4), b[i]);
    system.InvalidateCacheRange(g.cave, 0x80);

    const auto patch = [&](VAddr site, u32 original, VAddr stub) {
        const u32 word = enabled ? Branch(0xEB000000, site, stub) : original;
        if (memory.Read32(*process, site) != word) {
            memory.Write32(*process, site, word);
            system.InvalidateCacheRange(site, 4);
        }
    };
    for (const auto& [site, original] : g.pickup_sites)
        patch(site, original, g.cave);
    for (const auto& site : g.gate_sites)
        patch(site.addr, site.original, g.cave + StubB);
    LOG_INFO(Core, "Leaf: auto-stack {}", enabled ? "on" : "off");
}

bool HandleSvc(Core::System& system, u32 immediate) {
    if (immediate != LeafSvc || !g.ok)
        return false;
    auto& core = system.GetRunningCore();
    // Dynarmic reports the instruction after the SVC, the interpreter the SVC itself.
    const VAddr pc = core.GetPC();
    const bool stub_a = pc == g.cave || pc == g.cave + 4;
    const bool stub_b = pc == g.cave + StubB || pc == g.cave + StubB + 4;
    if (!stub_a && !stub_b)
        return false;

    auto& memory = system.Memory();
    const bool enabled = GetConfig().auto_stack.load();
    core.SetReg(12, 0);
    if (!enabled)
        return true;

    if (stub_a) {
        const VAddr pockets = core.GetReg(0);
        const VAddr item = core.GetReg(1);
        if (!TryStack(memory, pockets, item))
            return true;
        if (core.GetReg(3) != 0) {
            core.SetReg(0, pockets + NotifyOffset);
            core.SetReg(2, 0);
            core.SetReg(12, 1);
        } else {
            core.SetReg(12, 2);
        }
        return true;
    }

    const VAddr site = core.GetReg(14) - 4;
    const auto it = std::find_if(g.gate_sites.begin(), g.gate_sites.end(),
                                 [site](const GateSite& s) { return s.addr == site; });
    if (it == g.gate_sites.end())
        return true;
    const VAddr item = it->on_stack ? core.GetReg(13) + it->reg_or_offset
                                    : core.GetReg(static_cast<int>(it->reg_or_offset));
    if (const auto slot = StackSlotFor(memory, core.GetReg(0), item)) {
        core.SetReg(0, static_cast<u32>(*slot));
        core.SetReg(12, 1);
    }
    return true;
}

} // namespace Leaf::PocketStacks
