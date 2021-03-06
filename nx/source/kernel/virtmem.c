#include <switch.h>

typedef struct {
    u64  start;
    u64  end;
} VirtualRegion;

enum {
    REGION_STACK=0,
    REGION_HEAP=1,
    REGION_NEW_STACK=2,
    REGION_MAX
};

static VirtualRegion g_AddressSpace;
static VirtualRegion g_Region[REGION_MAX];
static u64 g_CurrentAddr;
static u64 g_CurrentMapAddr;
static Mutex g_VirtMemMutex;

static Result _GetRegionFromInfo(VirtualRegion* r, u64 id0_addr, u32 id0_sz) {
    u64 base;
    Result rc = svcGetInfo(&base, id0_addr, CUR_PROCESS_HANDLE, 0);

    if (R_SUCCEEDED(rc)) {
        u64 size;
        rc = svcGetInfo(&size, id0_sz, CUR_PROCESS_HANDLE, 0);

        if (R_SUCCEEDED(rc)) {
            r->start = base;
            r->end   = base + size;
        }
    }

    return rc;
}

static inline bool _InRegion(VirtualRegion* r, u64 addr) {
    return (addr >= r->start) && (addr < r->end);
}

void virtmemSetup(void) {
    if (R_FAILED(_GetRegionFromInfo(&g_AddressSpace, 12, 13))) {
        // Default values in case we're running on 1.0.0
        // Assumes 32-bit address space
        g_AddressSpace.start = 0ull;
        g_AddressSpace.end   = 0x100000000ull;
    }

    if (R_FAILED(_GetRegionFromInfo(&g_Region[REGION_STACK], 2, 3))) {
        fatalSimple(MAKERESULT(MODULE_LIBNX, LIBNX_BADGETINFO));
    }

    if (R_FAILED(_GetRegionFromInfo(&g_Region[REGION_HEAP], 4, 5))) {
        fatalSimple(MAKERESULT(MODULE_LIBNX, LIBNX_BADGETINFO));
    }

    // Failure is OK, happens on 1.0.0
    // In that case, g_UnkRegion will default to (0, 0).
    _GetRegionFromInfo(&g_Region[REGION_NEW_STACK], 14, 15);
}

void* virtmemReserve(size_t size) {
    Result rc;
    MemoryInfo meminfo;
    u32 pageinfo;
    size_t i;

    size = (size + 0xFFF) &~ 0xFFF;

    mutexLock(&g_VirtMemMutex);
    u64 addr = g_CurrentAddr;

    while (1)
    {
        // Add a guard page.
        addr += 0x1000;

        // If we go outside address space, let's go back to start.
        if (!_InRegion(&g_AddressSpace, addr)) {
            addr = g_AddressSpace.start;
        }
        // Query information about address.
        rc = svcQueryMemory(&meminfo, &pageinfo, addr);

        if (R_FAILED(rc)) {
            fatalSimple(MAKERESULT(MODULE_LIBNX, LIBNX_BADQUERYMEMORY));
        }

        if (meminfo.type != 0) {
            // Address is already taken, let's move past it.
            addr = meminfo.addr + meminfo.size;
            continue;
        }

        if (size > meminfo.size) {
            // We can't fit in this region, let's move past it.
            addr = meminfo.addr + meminfo.size;
            continue;
        }

        // Check if we end up in a reserved region.
        for(i=0; i<REGION_MAX; i++)
        {
            u64 end = addr + size - 1;

            if (_InRegion(&g_Region[i], addr)) {
                break;
            }

            if (_InRegion(&g_Region[i], end)) {
                break;
            }
        }

        // Did we?
        if (i != REGION_MAX) {
            addr = g_Region[i].end;
            continue;
        }

        // Not in a reserved region, we're good to go!
        break;
    }

    g_CurrentAddr = addr + size;

    mutexUnlock(&g_VirtMemMutex);
    return (void*) addr;
}

void  virtmemFree(void* addr, size_t size) {
    IGNORE_ARG(addr);
    IGNORE_ARG(size);
}

void* virtmemReserveMap(size_t size)
{
    Result rc;
    MemoryInfo meminfo;
    u32 pageinfo;

    int region_idx = kernelAbove200() ? REGION_NEW_STACK : REGION_STACK;
    size = (size + 0xFFF) &~ 0xFFF;

    mutexLock(&g_VirtMemMutex);
    u64 addr = g_CurrentMapAddr;

    while (1)
    {
        // Add a guard page.
        addr += 0x1000;

        // Make sure we stay inside the reserved map region.
        if (!_InRegion(&g_Region[region_idx], addr)) {
            addr = g_Region[region_idx].start;
        }

        // Query information about address.
        rc = svcQueryMemory(&meminfo, &pageinfo, addr);

        if (R_FAILED(rc)) {
            fatalSimple(MAKERESULT(MODULE_LIBNX, LIBNX_BADQUERYMEMORY));
        }

        if (meminfo.type != 0) {
            // Address is already taken, let's move past it.
            addr = meminfo.addr + meminfo.size;
            continue;
        }

        if (size > meminfo.size) {
            // We can't fit in this region, let's move past it.
            addr = meminfo.addr + meminfo.size;
            continue;
        }

        break;
    }

    g_CurrentMapAddr = addr + size;

    mutexUnlock(&g_VirtMemMutex);
    return (void*) addr;
}

void virtmemFreeMap(void* addr, size_t size) {
    IGNORE_ARG(addr);
    IGNORE_ARG(size);
}
