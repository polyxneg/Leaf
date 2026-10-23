// Copyright 2026 Poly
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Finding game code by signature and writing small ARM stubs (auto-stack, tool ring).

#pragma once

#include <optional>
#include <vector>
#include "common/common_types.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/hle/kernel/process.h"
#include "core/memory.h"

namespace Leaf::CodeSearch {

constexpr u32 BranchMask = 0xFF000000;
constexpr u32 OffsetMask = 0xFFFFF000; // ldr/str immediate offset, add immediate
constexpr u32 Exact = 0xFFFFFFFF;

struct Word {
    u32 value;
    u32 mask = Exact;
};
using Signature = std::vector<Word>;

inline std::vector<VAddr> FindAll(const std::vector<u32>& code, VAddr base,
                                  const Signature& sig) {
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

inline VAddr BranchTarget(VAddr at, u32 ins) {
    s32 offset = static_cast<s32>(ins << 8) >> 8;
    return at + 8 + static_cast<VAddr>(offset * 4);
}

inline u32 Branch(u32 cond_op, VAddr at, VAddr target) {
    return cond_op | (((target - (at + 8)) >> 2) & 0x00FFFFFF);
}

/// Literal loaded by `ldr rX, [pc, #imm]` at `at`.
inline std::optional<VAddr> LiteralAddress(VAddr at, u32 ins) {
    if ((ins & 0x0F7F0000) != 0x051F0000)
        return std::nullopt;
    const u32 imm = ins & 0xFFF;
    return (ins & (1u << 23)) ? at + 8 + imm : at + 8 - imm;
}

/// Value of an ARM data-processing immediate (8 bits rotated right by twice the top nibble).
inline u32 Immediate(u32 ins) {
    const u32 imm = ins & 0xFF;
    const u32 rotate = ((ins >> 8) & 0xF) * 2;
    return rotate == 0 ? imm : (imm >> rotate) | (imm << (32 - rotate));
}

class CodeView {
public:
    CodeView(Core::System& system, Kernel::Process& process, const char* feature_)
        : feature(feature_) {
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
            LOG_WARNING(Core, "Leaf: {}: {} found {} times", feature, what, hits.size());
            return std::nullopt;
        }
        return hits[0];
    }
    /// Start of the function containing `addr` (its `push {..., lr}`).
    std::optional<VAddr> FunctionStart(VAddr addr, u32 max_words = 256) const {
        for (u32 i = 0; i < max_words && Contains(addr); i++, addr -= 4) {
            if ((At(addr) & 0xFFFF4000) == 0xE92D4000)
                return addr;
        }
        return std::nullopt;
    }
    /// End of the code segment (after the last word).
    VAddr End() const {
        return base + static_cast<VAddr>(words.size() * 4);
    }

    VAddr base = 0;
    std::vector<u32> words;

private:
    const char* feature;
};

/// Value of a literal that may live in .rodata (outside the code view).
inline u32 ReadLiteral(Core::System& system, Kernel::Process& process, VAddr addr) {
    return system.Memory().Read32(process, addr);
}

} // namespace Leaf::CodeSearch
