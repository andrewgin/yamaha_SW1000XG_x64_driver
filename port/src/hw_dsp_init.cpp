/*
 * hw_dsp_init.cpp - DSP reset and microcode bring-up (M2).
 *
 * Replays the cold-start order over CSw1000Hw and the
 * loaded microcode. Runs at PASSIVE (StartDevice / D0 entry).
 *
 * Order: TRPIF=0 -> PORT0/PORT1 init -> settle -> reset pulse -> greg -> key on
 * -> MPR set 0 -> key off -> CESCR -> TRWF/TRWFO=0 -> unmute -> run -> IRQEN/RUN
 * in PORT1 -> DIT set-up. The interrupt is connected only afterwards (M3); with
 * TRPIF=0 nothing can fire yet.
 *
 * Deliberate deviations:
 *   TRPIF  cleared FIRST. The original cleared it (IrqDisableAll) only after
 *          setting PORT1.IRQEN, leaving a window with stale enables.
 *   - the 10 ms settle is a passive sleep, not a busy stall; the 44 us reset
 *     pulse stays as a stall (too short to sleep reliably).
 *   - every DSP transfer result is propagated; a failed download fails start.
 *   - PORT1 is modified per-write (each lock taken and released) instead of
 *     holding the PORT1 spin lock across the whole pageable sequence. Safe
 *     because bring-up is single-threaded.
 *
 * Clean-room; written from the notes.
 */

#include "sw1000_dsp.h"

/* PORT1 programming values (re/05 step 3): FS = 44.1 kHz, DSP0 out of reset,
 * CLDW idle high, plus bits 24/21/15 of unknown meaning (U-005). The "run"
 * variant adds IRQEN and RUN. Kept exactly as the original wrote them. */
#define SW_PORT1_INIT_VALUE   0x11A18000u
#define SW_PORT1_RUN_VALUE    0xD1A18000u

NTSTATUS
CSw1000Dsp::ColdStart()
{
    PAGED_CODE();
    if (m_mc == nullptr || !m_mc->IsLoaded()) {
        return STATUS_DEVICE_NOT_READY;
    }

    /* re/05 step 2 (IrqDisableAll), moved ahead of the PORT1.IRQEN write. */
    m_hw->WriteTrpifEnable(0);

    return InitDsp();
}

NTSTATUS
CSw1000Dsp::InitDsp()
{
    PAGED_CODE();

    m_hw->WritePort0(0x80);                         /* step 3.1 (meaning unknown) */
    m_hw->WritePort1(SW_PORT1_INIT_VALUE);          /* step 3.2 */
    m_diag->Log("  PORT0 <- 0x80, PORT1 <- 0x%08x; settle 10 ms", SW_PORT1_INIT_VALUE);

    /* step 3.3: 10 ms settle (passive sleep, H-015). */
    LARGE_INTEGER delay;
    delay.QuadPart = -100000LL;                     /* 10 ms, relative, 100-ns units */
    KeDelayExecutionThread(KernelMode, FALSE, &delay);

    NTSTATUS status = DspInitMprOnly(TRUE);         /* step 3.4 */
    if (!NT_SUCCESS(status)) {
        return status;
    }

    m_hw->WritePort1(SW_PORT1_RUN_VALUE);           /* step 3.5: + IRQEN + RUN */
    SetDitChannelStatus();                          /* then the DIT set-up */
    m_diag->Log("  PORT1 <- 0x%08x (IRQEN|RUN); DIT registers written", SW_PORT1_RUN_VALUE);
    return STATUS_SUCCESS;
}

NTSTATUS
CSw1000Dsp::DspInitMprOnly(_In_ BOOLEAN all)
{
    PAGED_CODE();

    /* step 4.1: the "32-bit MPR" flag is always set (ctor), nothing to do. */

    /* step 4.2: DSP-0 reset pulse. */
    m_hw->SetPort1Bits(SW_PORT1_MUTE);              /* mute */
    m_hw->ClearPort1Bits(SW_PORT1_NRES_DSP0);       /* assert reset */
    KeStallExecutionProcessor(44);                  /* 44 us pulse */
    m_hw->SetPort1Bits(SW_PORT1_NRES_DSP0);         /* release reset */
    m_resetTime = (LONGLONG)SwNow100ns();           /* H8 boot timestamp (H-014) */
    m_hw->ClearPort1Bits(SW_PORT1_IRQEN | SW_PORT1_RUN);
    m_diag->Log("  DSP0 reset pulse done (44 us)");

    NTSTATUS status = InitGreg(all);                /* step 4.3 */
    m_diag->Log("  greg ports 0-%lu: 0x%08x", (ULONG)(all ? SW_DSP_PORT_COUNT - 1 : 0), status);
    if (!NT_SUCCESS(status)) return status;

    status = m_hw->DspWriteWord(0, SW_SEL_MOD, SW_MOD_KEY_ADDR, SW_MOD_KEY_ON);   /* 4.4 */
    m_diag->Log("  key on: 0x%08x", status);
    if (!NT_SUCCESS(status)) return status;

    status = LoadMprSet0();                         /* step 4.5 */
    if (!NT_SUCCESS(status)) return status;

    status = m_hw->DspWriteWord(0, SW_SEL_MOD, SW_MOD_KEY_ADDR, SW_MOD_KEY_OFF);  /* 4.6 */
    m_diag->Log("  key off: 0x%08x", status);
    if (!NT_SUCCESS(status)) return status;

    status = SendCescr();                           /* step 4.7 */
    m_diag->Log("  CESCR: 0x%08x", status);
    if (!NT_SUCCESS(status)) return status;

    status = m_hw->RamWriteWord(SW_RAM_TRWF, 0);    /* step 4.8: no channel transfers */
    if (NT_SUCCESS(status)) {
        status = m_hw->RamWriteWord(SW_RAM_TRWFO, 0);
    }
    m_diag->Log("  TRWF/TRWFO <- 0: 0x%08x", status);
    if (!NT_SUCCESS(status)) return status;

    m_hw->ClearPort1Bits(SW_PORT1_MUTE);            /* step 4.9: unmute, then run */
    status = m_hw->DspWriteWord(0, SW_SEL_DSP_PARAM, SW_DSP_RUN_ADDR_32, SW_DSP_RUN_VALUE);
    m_diag->Log("  unmute; run word: 0x%08x", status);
    return status;
}

NTSTATUS
CSw1000Dsp::InitGreg(_In_ BOOLEAN all)
{
    /* SendGlobal for host ports 0..4 (all) or just port 0 (re/05 step 4.3).
     * On SW1000 "all" is used; ports 1..4 are written even though unused. */
    ULONG last = all ? (SW_DSP_PORT_COUNT - 1) : 0;
    for (ULONG n = 0; n <= last; ++n) {
        ULONG bytes = 0;
        const UCHAR *raw = m_mc->Blob((SW_BLOB)(SW_BLOB_N1GREG0 + n), &bytes);
        if (raw == nullptr || bytes < 0x20) {
            return STATUS_INVALID_IMAGE_FORMAT;
        }
        const USHORT *g = reinterpret_cast<const USHORT *>(raw);
        ULONG greg0 = ((ULONG)g[4] << 16) | g[5];
        ULONG greg1 = g[7];
        ULONG greg2 = ((ULONG)g[8]) << 16;
        m_hw->WritePortGlobal(n, greg0, greg1, greg2);
    }
    return STATUS_SUCCESS;
}

NTSTATUS
CSw1000Dsp::LoadMprSet0()
{
    for (ULONG slot = 0; slot < SW_MPR_SLOT_COUNT; ++slot) {
        SW_BLOB id = g_SwMprSlotBlob[slot];
        ULONG wc = 0;
        const ULONG *w = m_mc->BlobWords(id, &wc);
        if (w == nullptr || wc < g_SwMprSlotWords[slot]) {
            return STATUS_INVALID_IMAGE_FORMAT;
        }
        /* selector = slot << 8, DSP 0, address 0 (re/05 SendMpr). */
        NTSTATUS status = m_hw->DspWriteBlock(0, slot << 8, 0, w, g_SwMprSlotWords[slot]);
        m_diag->Log("  MPR slot %lu (%s, %lu words): 0x%08x",
                    slot, g_SwBlobs[id].name, g_SwMprSlotWords[slot], status);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS
CSw1000Dsp::SendCescr()
{
    ULONG wc = 0;
    const ULONG *w = m_mc->BlobWords(SW_BLOB_N1CREG0MEL, &wc);
    if (w == nullptr || wc < 6) {
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    return m_hw->DspWriteBlock(0, SW_SEL_CESCR, 0, w, 6);
}

/* ---- DIT (S/PDIF transmitter) set-up (re/05) ----------------------------- */

void
CSw1000Dsp::DitSendBit(_In_ BOOLEAN bit)
{
    /* Data on CDO0, then one CCK pulse. Each edge is one PORT1 write; the
     * original adds no delays between them. */
    if (bit) {
        m_hw->SetPort1Bits(SW_PORT1_CDO0);
    } else {
        m_hw->ClearPort1Bits(SW_PORT1_CDO0);
    }
    m_hw->SetPort1Bits(SW_PORT1_CCK);
    m_hw->ClearPort1Bits(SW_PORT1_CCK);
}

void
CSw1000Dsp::DitWriteRegister(_In_ ULONG reg, _In_ ULONG value)
{
    if (reg >= SW_DIT_REG_COUNT) {
        return;
    }

    m_hw->ClearPort1Bits(SW_PORT1_CLDW);            /* frame start */

    switch (reg) {
    case 0:                                         /* 32 bits, LSB first */
        for (ULONG i = 0; i < 32; ++i) {
            DitSendBit((BOOLEAN)((value >> i) & 1));
        }
        break;
    case 1:                                         /* 4 bits, MSB first */
        for (LONG i = 3; i >= 0; --i) {
            DitSendBit((BOOLEAN)((value >> i) & 1));
        }
        break;
    default:                                        /* 2: 32 bits, MSB first */
        for (LONG i = 31; i >= 0; --i) {
            DitSendBit((BOOLEAN)((value >> i) & 1));
        }
        break;
    }

    /* Two select bits = register number, MSB first. */
    DitSendBit((BOOLEAN)((reg >> 1) & 1));
    DitSendBit((BOOLEAN)(reg & 1));

    m_hw->SetPort1Bits(SW_PORT1_CLDW);              /* latch */
}

void
CSw1000Dsp::SetDitChannelStatus()
{
    /*
     * The original's set_sub_code(0,0,2,0) ignores its arguments and writes
     * these three fixed register values (re/05). What each bit means to the
     * transmitter chip is not known; the values are reproduced as-is.
     */
    DitWriteRegister(0, 0x204);
    DitWriteRegister(1, 0x8);
    DitWriteRegister(2, 0x0);
}
