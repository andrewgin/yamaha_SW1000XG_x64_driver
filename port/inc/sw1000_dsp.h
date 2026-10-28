/*
 * sw1000_dsp.h - DSP reset and microcode bring-up (M2).
 *
 * CSw1000Dsp replays the cold-start sequence over CSw1000Hw and the
 * loaded microcode. It is used at StartDevice and at every D0 entry
 * Clean-room; written from the notes.
 */

#ifndef SW1000_DSP_H
#define SW1000_DSP_H

#include "sw1000_hw.h"
#include "sw1000_blobs.h"
#include "diag.h"

/* H8/XG engine boot window after the DSP reset pulse (re/04 WaitH8, H-014).
 * 10 s in 100-ns units. The adapter keeps the reset timestamp; the mixer path
 * (M6) checks it without blocking. */
#define SW_H8_READY_100NS  (100000000LL)

class CSw1000Dsp
{
public:
    /* Each step is logged to diag, so a failed start names the step. */
    CSw1000Dsp(_In_ CSw1000Hw *hw, _In_ CSw1000Microcode *mc, _In_ CSwDiag *diag)
        : m_hw(hw), m_mc(mc), m_diag(diag) {}

    /* Full cold start (re/05 steps 2-4). Returns failure if any DSP transfer or
     * microcode download fails (H-017), unlike the original which ignored them. */
    NTSTATUS ColdStart();

    /* When the DSP reset pulse ended (SwNow100ns units). */
    LONGLONG ResetTime100ns() const { return m_resetTime; }

private:
    NTSTATUS InitDsp();                         /* re/05 step 3 */
    NTSTATUS DspInitMprOnly(_In_ BOOLEAN all);  /* re/05 step 4 */
    NTSTATUS InitGreg(_In_ BOOLEAN all);
    NTSTATUS LoadMprSet0();
    NTSTATUS SendCescr();

    /* S/PDIF transmitter set-up over the PORT1 serial pins (re/05). */
    void SetDitChannelStatus();
    void DitWriteRegister(_In_ ULONG reg, _In_ ULONG value);
    void DitSendBit(_In_ BOOLEAN bit);

    CSw1000Hw        *m_hw;
    CSw1000Microcode *m_mc;
    CSwDiag          *m_diag;
    LONGLONG          m_resetTime = 0;
};

#endif /* SW1000_DSP_H */
