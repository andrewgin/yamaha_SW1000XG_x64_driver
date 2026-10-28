/*
 * hw_uart.cpp - polled 8251 UART access (M7 part 1).
 *
 * Source: re/04 UART table (init sequence, command shadow, error reset),
 * re/14 (36 us after command writes, 13 us after data accesses, one spin lock
 * for command/status access outside the ISR). Clean-room; no Yamaha code.
 *
 * Deviation: the original's transmit waits were iteration counts with no
 * time bound (H-015); here every wait is bounded in microseconds.
 */

#include "sw1000_uart.h"

void
CSw1000Uart::Attach(_In_ CSw1000Hw *hw)
{
    m_hw = hw;
    KeInitializeSpinLock(&m_lock);
}

void
CSw1000Uart::WriteCommandLocked(_In_ ULONG ch, _In_ UCHAR cmd)
{
    m_cmd[ch] = cmd;
    Sw_Wr8(m_hw->BarBase(), Base(ch) + 1, cmd);
    KeStallExecutionProcessor(SW_UART_CMD_DELAY_US);
}

void
CSw1000Uart::ModifyCommand(_In_ ULONG ch, _In_ UCHAR clearMask, _In_ UCHAR setMask)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_lock, &irql);
    WriteCommandLocked(ch, (UCHAR)((m_cmd[ch] & ~clearMask) | setMask));
    KeReleaseSpinLock(&m_lock, irql);
}

void
CSw1000Uart::Init(_In_ ULONG ch)
{
    static const UCHAR kInit[] = {
        0x00, 0x00, 0x00,                                   /* reach the command state */
        SW_UART_CMD_INT_RESET | SW_UART_CMD_ERR_RESET,      /* internal reset */
        SW_UART_MODE_MIDI,                                  /* mode */
        SW_UART_CMD_ERR_RESET,                              /* command: clear errors */
    };
    KIRQL irql;
    KeAcquireSpinLock(&m_lock, &irql);
    for (ULONG i = 0; i < ARRAYSIZE(kInit); ++i) {
        WriteCommandLocked(ch, kInit[i]);
    }
    KeReleaseSpinLock(&m_lock, irql);
}

UCHAR
CSw1000Uart::Status(_In_ ULONG ch)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_lock, &irql);
    UCHAR status = Sw_Rd8(m_hw->BarBase(), Base(ch) + 1);
    if (status & SW_UART_ST_ERRORS) {
        WriteCommandLocked(ch, (UCHAR)(m_cmd[ch] | SW_UART_CMD_ERR_RESET));
    }
    KeReleaseSpinLock(&m_lock, irql);
    return status;
}

NTSTATUS
CSw1000Uart::WriteBytePolled(_In_ ULONG ch, _In_ UCHAR value, _In_ ULONG timeoutUs)
{
    ULONG64 deadline = SwNow100ns() + (ULONG64)timeoutUs * 10;
    for (;;) {
        KIRQL irql;
        KeAcquireSpinLock(&m_lock, &irql);
        UCHAR status = Sw_Rd8(m_hw->BarBase(), Base(ch) + 1);
        if (status & SW_UART_ST_TXRDY) {
            Sw_Wr8(m_hw->BarBase(), Base(ch), value);
            KeStallExecutionProcessor(SW_UART_DATA_DELAY_US);
            KeReleaseSpinLock(&m_lock, irql);
            return STATUS_SUCCESS;
        }
        KeReleaseSpinLock(&m_lock, irql);
        if (SwNow100ns() > deadline) {
            return STATUS_IO_TIMEOUT;
        }
        KeStallExecutionProcessor(5);
    }
}

NTSTATUS
CSw1000Uart::ReadBytePolled(_In_ ULONG ch, _Out_ PUCHAR value, _In_ ULONG timeoutUs)
{
    *value = 0;
    ULONG64 deadline = SwNow100ns() + (ULONG64)timeoutUs * 10;
    for (;;) {
        KIRQL irql;
        KeAcquireSpinLock(&m_lock, &irql);
        UCHAR status = Sw_Rd8(m_hw->BarBase(), Base(ch) + 1);
        if (status & SW_UART_ST_RXRDY) {
            *value = Sw_Rd8(m_hw->BarBase(), Base(ch));
            KeStallExecutionProcessor(SW_UART_DATA_DELAY_US);
            if (status & SW_UART_ST_ERRORS) {
                WriteCommandLocked(ch, (UCHAR)(m_cmd[ch] | SW_UART_CMD_ERR_RESET));
            }
            KeReleaseSpinLock(&m_lock, irql);
            return (status & SW_UART_ST_ERRORS) ? STATUS_DATA_ERROR : STATUS_SUCCESS;
        }
        KeReleaseSpinLock(&m_lock, irql);
        if (SwNow100ns() > deadline) {
            return STATUS_IO_TIMEOUT;
        }
        KeStallExecutionProcessor(5);
    }
}
