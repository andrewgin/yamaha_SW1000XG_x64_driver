/*
 * hw_hostport.cpp - DSP host-port transaction protocol (M1).
 *
 * Source: "Host-port protocol" (the exact CTRL/ADDR/DATA
 * sequences for word read/write, RAM read/write, and block write) and re/14
 * (serialization + bounded waits). The CTRL word carries bits23:16 = word count
 * and bits15:0 = address[31:16]; ADDR carries address[15:0] and the selector.
 *
 * Deviations from the original (intentional):
 *   - the 0xfffff iteration BUSY poll becomes a microsecond-bounded wait.
 *   - timeouts are returned, not ignored, so callers can fail cleanly.
 *
 * Clean-room; no Yamaha code.
 */

#include "sw1000_hw.h"

/*
 * Serialize a host-port transaction. For now every port uses its own spin lock.
 * M3 replaces the port-0 case with IInterruptSync::CallSynchronizedRoutine so it
 * cannot race the ISR's RAM accesses (re/04, re/14).
 */
void
CSw1000Hw::AcquireHostPort(_In_ ULONG port, _Out_ PKIRQL irql)
{
    KeAcquireSpinLock(&m_lockHostPort[port], irql);
}

void
CSw1000Hw::ReleaseHostPort(_In_ ULONG port, _In_ KIRQL irql)
{
    KeReleaseSpinLock(&m_lockHostPort[port], irql);
}

NTSTATUS
CSw1000Hw::WaitHostPortReady(_In_ ULONG port)
{
    ULONG ctrlOff = Sw_DspPort(port) + SW_DSP_OFF_CTRL;
    for (ULONG us = 0; us < SW_HOSTPORT_TIMEOUT_US; ++us) {
        if ((Sw_Rd32(m_bar, ctrlOff) & SW_DSP_CTRL_BUSY) == 0) {
            if (us > m_maxBusyUs) {
                m_maxBusyUs = us;
            }
            return STATUS_SUCCESS;
        }
        KeStallExecutionProcessor(1);
    }
    m_busyTimeouts++;
    SwTrace("host port %u BUSY timeout\n", port);
    return STATUS_IO_TIMEOUT;
}

/* ---- locked cores ------------------------------------------------------- */

NTSTATUS
CSw1000Hw::DspWriteWordLocked(ULONG port, ULONG sel, ULONG addr, ULONG value)
{
    NTSTATUS status = WaitHostPortReady(port);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    ULONG b = Sw_DspPort(port);
    Sw_Wr32(m_bar, b + SW_DSP_OFF_DATA, value);
    Sw_Wr32(m_bar, b + SW_DSP_OFF_CTRL, SW_DSP_CTRL_COUNT1 | (addr >> 16));
    Sw_Wr32(m_bar, b + SW_DSP_OFF_ADDR, ((addr & 0xFFFFu) << 16) | (sel & 0xFFFFu));
    return STATUS_SUCCESS;
}

NTSTATUS
CSw1000Hw::DspReadWordLocked(ULONG port, ULONG sel, ULONG addr, PULONG value)
{
    NTSTATUS status = WaitHostPortReady(port);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    ULONG b = Sw_DspPort(port);
    Sw_Wr32(m_bar, b + SW_DSP_OFF_CTRL, SW_DSP_CTRL_COUNT1 | (addr >> 16));
    Sw_Wr32(m_bar, b + SW_DSP_OFF_ADDR, ((addr & 0xFFFFu) << 16) | (sel & 0xFFFFu));

    status = WaitHostPortReady(port);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    Sw_Wr32(m_bar, b + SW_DSP_OFF_CTRL, SW_DSP_CTRL_FLAG | SW_DSP_CTRL_COUNT1 | (addr >> 16));
    *value = Sw_Rd32(m_bar, b + SW_DSP_OFF_DATA);
    Sw_Wr32(m_bar, b + SW_DSP_OFF_CTRL, SW_DSP_CTRL_COUNT1 | (addr >> 16));
    return STATUS_SUCCESS;
}

NTSTATUS
CSw1000Hw::RamWriteWordLocked(ULONG addr, ULONG value)
{
    NTSTATUS status = WaitHostPortReady(0);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    ULONG b = Sw_DspPort(0);
    /* RAM single write brackets the data write with the bit-24 flag (re/04). */
    Sw_Wr32(m_bar, b + SW_DSP_OFF_CTRL, SW_DSP_CTRL_FLAG | SW_DSP_CTRL_COUNT1 | (addr >> 16));
    Sw_Wr32(m_bar, b + SW_DSP_OFF_DATA, value);
    Sw_Wr32(m_bar, b + SW_DSP_OFF_CTRL, SW_DSP_CTRL_COUNT1 | (addr >> 16));
    Sw_Wr32(m_bar, b + SW_DSP_OFF_ADDR, ((addr & 0xFFFFu) << 16) | SW_SEL_RAM_WRITE);
    return STATUS_SUCCESS;
}

NTSTATUS
CSw1000Hw::DspWriteBlockLocked(ULONG port, ULONG sel, ULONG addr, const ULONG *buf, ULONG count)
{
    ULONG b = Sw_DspPort(port);
    while (count != 0) {
        ULONG n = (count < SW_DSP_DATA_WORDS) ? count : SW_DSP_DATA_WORDS;

        NTSTATUS status = WaitHostPortReady(port);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        for (ULONG i = 0; i < n; ++i) {
            Sw_Wr32(m_bar, b + SW_DSP_OFF_DATA + i * sizeof(ULONG), buf[i]);
        }
        Sw_Wr32(m_bar, b + SW_DSP_OFF_CTRL, (n << 16) | (addr >> 16));
        Sw_Wr32(m_bar, b + SW_DSP_OFF_ADDR, ((addr & 0xFFFFu) << 16) | (sel & 0xFFFFu));

        addr  += n;
        buf   += n;
        count -= n;
    }
    return STATUS_SUCCESS;
}

/* ---- public wrappers (serialize, then run the core) --------------------- *
 * Transactions are limited to SW_DSP_PORTS_PRESENT: on the SW1000XG ports 1-4
 * read back all-ones (hardware round 1), so their BUSY bit never clears and a
 * transaction would always time out. Their GREG writes stay (WritePortGlobal):
 * the original makes them, and a write to an empty port is harmless.
 */

NTSTATUS
CSw1000Hw::DspWriteWord(_In_ ULONG port, _In_ ULONG sel, _In_ ULONG addr, _In_ ULONG value)
{
    if (port >= SW_DSP_PORTS_PRESENT) {
        return STATUS_INVALID_PARAMETER;
    }
    KIRQL irql;
    AcquireHostPort(port, &irql);
    NTSTATUS status = DspWriteWordLocked(port, sel, addr, value);
    ReleaseHostPort(port, irql);
    return status;
}

NTSTATUS
CSw1000Hw::DspReadWord(_In_ ULONG port, _In_ ULONG sel, _In_ ULONG addr, _Out_ PULONG value)
{
    *value = 0;
    if (port >= SW_DSP_PORTS_PRESENT) {
        return STATUS_INVALID_PARAMETER;
    }
    KIRQL irql;
    AcquireHostPort(port, &irql);
    NTSTATUS status = DspReadWordLocked(port, sel, addr, value);
    ReleaseHostPort(port, irql);
    return status;
}

NTSTATUS
CSw1000Hw::DspWriteBlock(_In_ ULONG port, _In_ ULONG sel, _In_ ULONG addr,
                         _In_reads_(count) const ULONG *buf, _In_ ULONG count)
{
    if (port >= SW_DSP_PORTS_PRESENT || (count != 0 && buf == nullptr)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (count == 0) {
        return STATUS_SUCCESS;
    }
    KIRQL irql;
    AcquireHostPort(port, &irql);
    NTSTATUS status = DspWriteBlockLocked(port, sel, addr, buf, count);
    ReleaseHostPort(port, irql);
    return status;
}

void
CSw1000Hw::WritePortGlobal(_In_ ULONG port, _In_ ULONG greg0, _In_ ULONG greg1, _In_ ULONG greg2)
{
    if (port >= SW_DSP_PORT_COUNT) {
        return;
    }
    ULONG b = Sw_DspPort(port);
    KIRQL irql;
    AcquireHostPort(port, &irql);
    /* Direct writes (not a host-port transaction); no BUSY wait (re/05). */
    Sw_Wr32(m_bar, b + SW_DSP_OFF_GREG0, greg0);
    Sw_Wr32(m_bar, b + SW_DSP_OFF_GREG1, greg1);
    Sw_Wr32(m_bar, b + SW_DSP_OFF_GREG2, greg2);
    ReleaseHostPort(port, irql);
}

NTSTATUS
CSw1000Hw::RamWriteWord(_In_ ULONG addr, _In_ ULONG value)
{
    KIRQL irql;
    AcquireHostPort(0, &irql);
    NTSTATUS status = RamWriteWordLocked(addr, value);
    ReleaseHostPort(0, irql);
    return status;
}

NTSTATUS
CSw1000Hw::RamReadWord(_In_ ULONG addr, _Out_ PULONG value)
{
    *value = 0;
    KIRQL irql;
    AcquireHostPort(0, &irql);
    NTSTATUS status = DspReadWordLocked(0, SW_SEL_RAM_READ, addr, value);
    ReleaseHostPort(0, irql);
    return status;
}

NTSTATUS
CSw1000Hw::RamWriteBlock(_In_ ULONG addr, _In_reads_(count) const ULONG *buf, _In_ ULONG count)
{
    /*
     * Not the same as DspWriteBlock (re/04 corrected): the original RAM block
     * write is a single burst and brackets the data phase with the bit-24 flag,
     * exactly like the single-word RAM write.
     */
    if (count == 0) {
        return STATUS_SUCCESS;
    }
    if (buf == nullptr || count > SW_DSP_DATA_WORDS) {
        return STATUS_INVALID_PARAMETER;
    }

    KIRQL irql;
    AcquireHostPort(0, &irql);

    NTSTATUS status = WaitHostPortReady(0);
    if (NT_SUCCESS(status)) {
        ULONG b = Sw_DspPort(0);
        Sw_Wr32(m_bar, b + SW_DSP_OFF_CTRL, SW_DSP_CTRL_FLAG | (count << 16) | (addr >> 16));
        for (ULONG i = 0; i < count; ++i) {
            Sw_Wr32(m_bar, b + SW_DSP_OFF_DATA + i * sizeof(ULONG), buf[i]);
        }
        Sw_Wr32(m_bar, b + SW_DSP_OFF_CTRL, (count << 16) | (addr >> 16));
        Sw_Wr32(m_bar, b + SW_DSP_OFF_ADDR, ((addr & 0xFFFFu) << 16) | SW_SEL_RAM_WRITE);
    }

    ReleaseHostPort(0, irql);
    return status;
}
