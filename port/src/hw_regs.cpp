/*
 * hw_regs.cpp - BAR mapping and the shadowed control registers (M1).
 *
 * Clean-room; no Yamaha code.
 */

#include "sw1000_hw.h"

CSw1000Hw::~CSw1000Hw()
{
    UnmapBar();
}

NTSTATUS
CSw1000Hw::MapBar(_In_ PHYSICAL_ADDRESS Physical, _In_ ULONG Length)
{
    PAGED_CODE();

    if (m_bar != nullptr) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    /* We now know the top register offset (0x3ff10), so enforce a minimum the
     * original never checked (re/04, U-006). */
    if (Length < SW_BAR_MIN_LENGTH) {
        SwTrace("MapBar: BAR too small (0x%x < 0x%x)\n", Length, SW_BAR_MIN_LENGTH);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    KeInitializeSpinLock(&m_lockPort0);
    KeInitializeSpinLock(&m_lockPort1);
    KeInitializeSpinLock(&m_lockTrpif);
    KeInitializeSpinLock(&m_lockMks);
    for (ULONG i = 0; i < SW_DSP_PORT_COUNT; ++i) {
        KeInitializeSpinLock(&m_lockHostPort[i]);
    }

    m_barPhysical = Physical;
    m_barLength   = Length;
    m_bar = MmMapIoSpaceEx(Physical, Length, PAGE_READWRITE | PAGE_NOCACHE);
    if (m_bar == nullptr) {
        SwTrace("MapBar: MmMapIoSpaceEx failed (len=0x%x)\n", Length);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    SwTrace("MapBar: phys=0x%llx len=0x%x -> %p\n",
            (ULONGLONG)Physical.QuadPart, Length, m_bar);
    return STATUS_SUCCESS;
}

void
CSw1000Hw::UnmapBar()
{
    if (m_bar != nullptr) {
        MmUnmapIoSpace(m_bar, m_barLength);
        m_bar = nullptr;
    }
}

void
CSw1000Hw::Quiesce()
{
    if (m_bar == nullptr) {
        return;
    }
    /* Sources off first, then the master enable/run, then mute. Writes to a
     * surprise-removed device are dropped by the bus, so this is always safe. */
    WriteTrpifEnable(0);
    ModifyPort1(SW_PORT1_IRQEN | SW_PORT1_RUN, SW_PORT1_MUTE);
}

/* ---- PORT1 (write-only, shadowed) --------------------------------------- */

void
CSw1000Hw::WritePort1(_In_ ULONG value)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_lockPort1, &irql);
    m_shadowPort1 = value;
    Sw_Wr32(m_bar, SW_REG_PORT1, value);
    KeReleaseSpinLock(&m_lockPort1, irql);
}

void
CSw1000Hw::ModifyPort1(_In_ ULONG clearMask, _In_ ULONG setMask)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_lockPort1, &irql);
    m_shadowPort1 = (m_shadowPort1 & ~clearMask) | setMask;
    Sw_Wr32(m_bar, SW_REG_PORT1, m_shadowPort1);
    KeReleaseSpinLock(&m_lockPort1, irql);
}

/* ---- PORT0 (config read / control write, shadowed on write) ------------- */

void
CSw1000Hw::WritePort0(_In_ ULONG value)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_lockPort0, &irql);
    m_shadowPort0 = value;
    Sw_Wr32(m_bar, SW_REG_PORT0, value);
    KeReleaseSpinLock(&m_lockPort0, irql);
}

ULONG
CSw1000Hw::ReadPort0()
{
    /* Board-configuration bits are read live from hardware (re/04). */
    return Sw_Rd32(m_bar, SW_REG_PORT0);
}

/* ---- TRPIF (irq enable write / status read) ----------------------------- *
 * M1 writes the enable mask under its own lock. M3 will route the read-modify
 * write through IInterruptSync, since the ISR also updates interrupt state.
 */

void
CSw1000Hw::WriteTrpifEnable(_In_ ULONG value)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_lockTrpif, &irql);
    m_shadowTrpif = value;
    Sw_Wr32(m_bar, SW_REG_TRPIF, value);
    KeReleaseSpinLock(&m_lockTrpif, irql);
}

void
CSw1000Hw::ModifyTrpifEnable(_In_ ULONG clearMask, _In_ ULONG setMask)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_lockTrpif, &irql);
    m_shadowTrpif = (m_shadowTrpif & ~clearMask) | setMask;
    Sw_Wr32(m_bar, SW_REG_TRPIF, m_shadowTrpif);
    KeReleaseSpinLock(&m_lockTrpif, irql);
}

ULONG
CSw1000Hw::ReadTrpifStatus()
{
    /* Read semantics (does a read clear pending bits?) are U-004, needs-hw. */
    return Sw_Rd32(m_bar, SW_REG_TRPIF);
}

/* ---- MKS (opaque 32-bit) ------------------------------------------------ */

void
CSw1000Hw::WriteMks(_In_ ULONG value)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_lockMks, &irql);
    Sw_Wr32(m_bar, SW_REG_MKS, value);
    KeReleaseSpinLock(&m_lockMks, irql);
}

ULONG
CSw1000Hw::ReadMks()
{
    KIRQL irql;
    KeAcquireSpinLock(&m_lockMks, &irql);
    ULONG v = Sw_Rd32(m_bar, SW_REG_MKS);
    KeReleaseSpinLock(&m_lockMks, irql);
    return v;
}
