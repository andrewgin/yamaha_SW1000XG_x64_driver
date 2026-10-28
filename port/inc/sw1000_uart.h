/*
 * sw1000_uart.h - the two 8251-style MIDI UARTs (M7, re/04 UART table, re/10).
 *
 * Channel 0 is the MIDI connector (MIDIOUT/MIDIIN), channel 1 feeds the
 * on-card XG synth (SWXG1-3). Each channel is a byte-wide data register at
 * base+0 and a command (write) / status (read) register at base+1.
 *
 * This is the polled layer used by bring-up and the diagnostic stages. The
 * interrupt-driven transmit/receive paths (TRPIF bits 24-27) arrive with M3
 * and M7 part 2 and will share the command shadow and lock kept here.
 *
 * Clean-room; written from the notes.
 */

#ifndef SW1000_UART_H
#define SW1000_UART_H

#include "sw1000_hw.h"

/* Register recovery times the original keeps after every access (re/14). */
#define SW_UART_CMD_DELAY_US    36u
#define SW_UART_DATA_DELAY_US   13u

class CSw1000Uart
{
public:
    CSw1000Uart() = default;

    void Attach(_In_ CSw1000Hw *hw);

    /* 8251 reset + mode (re/04): three dummy commands, internal reset, mode
     * 0x4E, error reset. Leaves the command shadow at 0x10. */
    void Init(_In_ ULONG ch);
    void EnableTx(_In_ ULONG ch)  { ModifyCommand(ch, 0, SW_UART_CMD_TXEN); }
    void EnableRx(_In_ ULONG ch)  { ModifyCommand(ch, 0, SW_UART_CMD_RXEN); }
    void DisableRx(_In_ ULONG ch) { ModifyCommand(ch, SW_UART_CMD_RXEN, 0); }
    UCHAR Command(_In_ ULONG ch) const { return m_cmd[ch]; }

    /* Status as read. Like the original, a status showing a parity, overrun
     * or framing error is followed by an error-reset command. */
    UCHAR Status(_In_ ULONG ch);

    /* ISR forms (DIRQL, under the interrupt-sync lock; no spin lock): a plain
     * status read, and a data write followed by the 13 us recovery time. */
    UCHAR StatusIsr(_In_ ULONG ch) { return Sw_Rd8(m_hw->BarBase(), Base(ch) + 1); }
    void  WriteDataIsr(_In_ ULONG ch, _In_ UCHAR value)
    {
        Sw_Wr8(m_hw->BarBase(), Base(ch), value);
        KeStallExecutionProcessor(SW_UART_DATA_DELAY_US);
    }

    UCHAR ReadDataIsr(_In_ ULONG ch)
    {
        UCHAR value = Sw_Rd8(m_hw->BarBase(), Base(ch));
        KeStallExecutionProcessor(SW_UART_DATA_DELAY_US);
        return value;
    }
    /* Error reset from the ISR (the shadow keeps the TX/RX enables). */
    void ErrorResetIsr(_In_ ULONG ch) { WriteCommandLocked(ch, (UCHAR)(m_cmd[ch] | SW_UART_CMD_ERR_RESET)); }

    /* Polled byte I/O, bounded by TimeoutUs (PASSIVE or DISPATCH). Not used
     * on a channel once its interrupt-driven path is running. */
    NTSTATUS WriteBytePolled(_In_ ULONG ch, _In_ UCHAR value, _In_ ULONG timeoutUs);
    NTSTATUS ReadBytePolled(_In_ ULONG ch, _Out_ PUCHAR value, _In_ ULONG timeoutUs);

private:
    void WriteCommandLocked(_In_ ULONG ch, _In_ UCHAR cmd);
    void ModifyCommand(_In_ ULONG ch, _In_ UCHAR clearMask, _In_ UCHAR setMask);
    ULONG Base(_In_ ULONG ch) const { return ch == 0 ? SW_REG_UART0_DATA : SW_REG_UART1_DATA; }

    CSw1000Hw *m_hw = nullptr;
    UCHAR      m_cmd[SW_UART_CHANNELS] = {};      /* last command byte written */
    KSPIN_LOCK m_lock;                            /* command/status/data access */
};

#endif /* SW1000_UART_H */
