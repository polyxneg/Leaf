// Copyright 2026 Poly
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Reverse-engineering hooks for the Leaf harness: memory write watchpoints and execution
// breakpoints in the DynCom interpreter. Only compiled when LEAF_DEBUG_HOOKS is defined (harness
// builds); release builds contain none of this.

#pragma once

#ifdef LEAF_DEBUG_HOOKS

#include <array>
#include <functional>
#include <unordered_set>
#include "common/common_types.h"

namespace Leaf::Debug {

using Regs = std::array<u32, 16>;

struct Hooks {
    bool active = false;
    VAddr watch_begin = 0, watch_end = 0; ///< [begin, end) range for write callbacks
    std::function<void(VAddr addr, u32 size, u64 value, const Regs& regs)> on_write;
    VAddr read_begin = 0, read_end = 0; ///< [begin, end) range for read callbacks
    std::function<void(VAddr addr, u32 size, const Regs& regs)> on_read;
    std::unordered_set<u32> exec; ///< instruction addresses for exec callbacks
    std::function<void(const Regs& regs)> on_exec;
};

Hooks& GetHooks();

inline void OnWrite(VAddr addr, u32 size, u64 value, const Regs& regs) {
    auto& h = GetHooks();
    if (h.active && addr + size > h.watch_begin && addr < h.watch_end && h.on_write)
        h.on_write(addr, size, value, regs);
}

inline void OnRead(VAddr addr, u32 size, const Regs& regs) {
    auto& h = GetHooks();
    if (h.active && addr + size > h.read_begin && addr < h.read_end && h.on_read)
        h.on_read(addr, size, regs);
}

} // namespace Leaf::Debug

#endif
