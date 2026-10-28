/*
 * sw1000_hw.h - the hardware access layer (register shadows + host-port protocol).
 *
 * CSw1000Hw owns the mapped BAR, the PORT0/PORT1/TRPIF/MKS software shadows and
 * their locks, and the DSP host-port transaction helpers. It is the foundation
 * every higher layer calls (re/04, re/14). It is a plain C++ object (not COM),
 * owned by the adapter.
 *
 * Lock discipline (re/14, H-016): one owner per shadowed register; the three
 * level hierarchy is mutex -> spin lock -> interrupt lock. Host-port port 0 will
 * serialize through IInterruptSync once the ISR exists (M3); until then, and for
 * ports 1..4 always, a per-port spin lock serializes (see AcquireHostPort).
 */

#ifndef SW1000_HW_H
#define SW1000_HW_H

#include "sw1000_regs.h"

/*
 * Host-port BUSY timeout. The original polled CTRL bit 31 up to 0xfffff times
 * with no delay; we bound the wait in microseconds instead (H-015). The spin is
 * at DISPATCH (a lock is held), so keep the budget modest; normal completion is
 * immediate. Tune against hardware.
 */
#define SW_HOSTPORT_TIMEOUT_US  2000u

class CSw1000Hw
{
public:
    CSw1000Hw() = default;
    ~CSw1000Hw();

    NTSTATUS MapBar(_In_ PHYSICAL_ADDRESS Physical, _In_ ULONG Length);
    void     UnmapBar();
    BOOLEAN  IsMapped() const { return m_bar != nullptr; }
    ULONG    BarLength() const { return m_barLength; }
    PVOID    BarBase() const { return m_bar; }

    /* ---- shadowed whole-register writes (one owner each) ---------------- */
    void  WritePort1(_In_ ULONG value);
    void  ModifyPort1(_In_ ULONG clearMask, _In_ ULONG setMask);
    void  SetPort1Bits(_In_ ULONG mask)   { ModifyPort1(0, mask); }
    void  ClearPort1Bits(_In_ ULONG mask) { ModifyPort1(mask, 0); }
    ULONG Port1Shadow() const { return m_shadowPort1; }

    void  WritePort0(_In_ ULONG value);
    ULONG ReadPort0();                       /* live board-config bits */

    void  WriteTrpifEnable(_In_ ULONG value);
    void  ModifyTrpifEnable(_In_ ULONG clearMask, _In_ ULONG setMask);
    ULONG TrpifShadow() const { return m_shadowTrpif; }
    /* Once the interrupt is connected, TRPIF enables change only here, under
     * the interrupt-sync lock (in the ISR or a synchronized routine). */
    void  ModifyTrpifEnableSynced(_In_ ULONG clearMask, _In_ ULONG setMask)
    {
        m_shadowTrpif = (m_shadowTrpif & ~clearMask) | setMask;
        Sw_Wr32(m_bar, SW_REG_TRPIF, m_shadowTrpif);
    }
    ULONG ReadTrpifStatus();                 /* M3 (ISR) */

    void  WriteMks(_In_ ULONG value);
    ULONG ReadMks();

    ULONG ReadScr() { return Sw_Rd32(m_bar, SW_REG_SCR); }

    /* Side-effect-free reads for diagnostics (and M7 for the UART status). */
    UCHAR ReadUartStatus(_In_ ULONG ch)
    { return Sw_Rd8(m_bar, ch == 0 ? SW_REG_UART0_CMD : SW_REG_UART1_CMD); }
    ULONG ReadHostPortCtrl(_In_ ULONG port)
    { return Sw_Rd32(m_bar, Sw_DspPort(port) + SW_DSP_OFF_CTRL); }

    /* Host-port BUSY statistics since mapping, for tuning the timeout (H-015).
     * Updated without a common lock across ports: diagnostic accuracy only. */
    ULONG MaxBusyWaitUs() const { return m_maxBusyUs; }
    ULONG BusyTimeouts() const { return m_busyTimeouts; }

    /* ---- host-port transactions (port 0..4) ---------------------------- */
    NTSTATUS DspWriteWord (_In_ ULONG port, _In_ ULONG sel, _In_ ULONG addr, _In_ ULONG value);
    NTSTATUS DspReadWord  (_In_ ULONG port, _In_ ULONG sel, _In_ ULONG addr, _Out_ PULONG value);
    NTSTATUS DspWriteBlock(_In_ ULONG port, _In_ ULONG sel, _In_ ULONG addr,
                           _In_reads_(count) const ULONG *buf, _In_ ULONG count);

    /* Global registers of host port n (+0x88/+0x8c/+0x90), direct, locked.
     * Used by the DSP bring-up SendGlobal step (re/05). */
    void WritePortGlobal(_In_ ULONG port, _In_ ULONG greg0, _In_ ULONG greg1, _In_ ULONG greg2);

    /* ---- "RAM" stream-register file (port 0, selectors 0xe00/0xf00) ----- */
    NTSTATUS RamWriteWord (_In_ ULONG addr, _In_ ULONG value);
    NTSTATUS RamReadWord  (_In_ ULONG addr, _Out_ PULONG value);
    /* One burst of 1..32 words, bracketed with the CTRL bit-24 flag like the
     * single-word RAM write (re/04 SendRAM). Larger counts are rejected. */
    NTSTATUS RamWriteBlock(_In_ ULONG addr, _In_reads_(count) const ULONG *buf, _In_ ULONG count);

    /* Put the card in a quiet state: all interrupt sources off, IRQEN/RUN
     * cleared, output muted. Used on teardown and failed start. */
    void Quiesce();

private:
    NTSTATUS WaitHostPortReady(_In_ ULONG port);

    /* cores run with the port already serialized */
    NTSTATUS DspWriteWordLocked (ULONG port, ULONG sel, ULONG addr, ULONG value);
    NTSTATUS DspReadWordLocked  (ULONG port, ULONG sel, ULONG addr, PULONG value);
    NTSTATUS DspWriteBlockLocked(ULONG port, ULONG sel, ULONG addr, const ULONG *buf, ULONG count);
    NTSTATUS RamWriteWordLocked (ULONG addr, ULONG value);

    /* M3 seam: port 0 will route through IInterruptSync here. */
    void AcquireHostPort(_In_ ULONG port, _Out_ PKIRQL irql);
    void ReleaseHostPort(_In_ ULONG port, _In_ KIRQL irql);

    PVOID            m_bar = nullptr;
    ULONG            m_barLength = 0;
    PHYSICAL_ADDRESS m_barPhysical = {};

    ULONG m_maxBusyUs = 0;
    ULONG m_busyTimeouts = 0;

    ULONG m_shadowPort0 = 0;
    ULONG m_shadowPort1 = 0;
    ULONG m_shadowTrpif = 0;

    KSPIN_LOCK m_lockPort0;
    KSPIN_LOCK m_lockPort1;
    KSPIN_LOCK m_lockTrpif;
    KSPIN_LOCK m_lockMks;
    KSPIN_LOCK m_lockHostPort[SW_DSP_PORT_COUNT];
};

#endif /* SW1000_HW_H */
