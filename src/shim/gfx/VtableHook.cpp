#include "VtableHook.h"

#include <atomic>

namespace retro::shim::gfx {
namespace {

constexpr int kMaxSlots = 64;
constexpr int kMaxVtables = 64;

struct PatchedVtable {
    void** vtable = nullptr;
    void* originals[kMaxSlots] = {};
    bool patched[kMaxSlots] = {};
};

// Append-only table: entries are fully written before `g_count` publishes
// them, so readers (every hooked call) need no lock.
PatchedVtable g_tables[kMaxVtables];
std::atomic<int> g_count{0};
SRWLOCK g_writeLock = SRWLOCK_INIT;

void** VtableOf(void* object) {
    return object ? *static_cast<void***>(object) : nullptr;
}

PatchedVtable* Find(void** vtable) {
    const int n = g_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        if (g_tables[i].vtable == vtable) return &g_tables[i];
    }
    return nullptr;
}

bool WriteSlot(void** slot, void* value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

}  // namespace

thread_local int InternalCall::depth_ = 0;

bool PatchVtable(void* object, std::initializer_list<SlotHook> hooks) {
    void** vtable = VtableOf(object);
    if (!vtable) return false;
    if (Find(vtable)) return true;

    AcquireSRWLockExclusive(&g_writeLock);
    bool ok = true;
    if (!Find(vtable)) {
        const int index = g_count.load(std::memory_order_relaxed);
        if (index >= kMaxVtables) {
            ok = false;
        } else {
            PatchedVtable& t = g_tables[index];
            t = PatchedVtable{};
            t.vtable = vtable;
            for (const SlotHook& h : hooks) {
                if (h.slot < 0 || h.slot >= kMaxSlots) continue;
                t.originals[h.slot] = vtable[h.slot];
            }
            g_count.store(index + 1, std::memory_order_release);  // publish originals first
            for (const SlotHook& h : hooks) {
                if (h.slot < 0 || h.slot >= kMaxSlots) continue;
                t.patched[h.slot] = WriteSlot(&vtable[h.slot], h.hook);
                ok = ok && t.patched[h.slot];
            }
        }
    }
    ReleaseSRWLockExclusive(&g_writeLock);
    return ok;
}

void* OriginalSlot(void* object, int slot) {
    if (slot < 0 || slot >= kMaxSlots) return nullptr;
    void** vtable = VtableOf(object);
    if (!vtable) return nullptr;  // (forgotten entries have a null vtable: never match them)
    if (const PatchedVtable* t = Find(vtable)) return t->originals[slot];
    return vtable[slot];  // not ours: the slot is still original
}

void ForgetVtablesInRange(const void* base, size_t size) {
    const auto begin = reinterpret_cast<uintptr_t>(base);
    AcquireSRWLockExclusive(&g_writeLock);
    const int n = g_count.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
        const auto addr = reinterpret_cast<uintptr_t>(g_tables[i].vtable);
        if (addr >= begin && addr < begin + size) g_tables[i].vtable = nullptr;
    }
    ReleaseSRWLockExclusive(&g_writeLock);
}

void RestoreAllVtables() {
    AcquireSRWLockExclusive(&g_writeLock);
    const int n = g_count.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
        PatchedVtable& t = g_tables[i];
        if (!t.vtable) continue;
        for (int s = 0; s < kMaxSlots; ++s) {
            if (t.patched[s]) WriteSlot(&t.vtable[s], t.originals[s]);
        }
        t.vtable = nullptr;
    }
    ReleaseSRWLockExclusive(&g_writeLock);
}

}  // namespace retro::shim::gfx
