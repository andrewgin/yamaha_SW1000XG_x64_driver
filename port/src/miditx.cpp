/*
 * miditx.cpp - MIDI transmit queue with XG port select (see sw1000_miditx.h).
 *
 * Non-paged: runs under the interrupt-sync lock. Clean-room; from re/10.
 */

#include "sw1000_miditx.h"

void
CSw1000MidiTx::Reset(_In_ BOOLEAN PortSelect)
{
    m_portSelect = PortSelect;
    m_head = 0;
    m_tail = 0;
    m_highWater = 0;
    m_lastPort = MAXULONG;
    m_f5Countdown = SW_F5_RESEND_COUNT;
    RtlZeroMemory(m_running, sizeof(m_running));
}

ULONG
CSw1000MidiTx::Enqueue(_In_ ULONG Port, _In_reads_(Length) const UCHAR *Data,
                       _In_ ULONG Length, _In_ BOOLEAN First)
{
    if (Port >= SW_MIDI_PORT_GROUPS || Length == 0 || (!m_portSelect && Port != 0)) {
        return 0;
    }

    /* Port select: on a change of port group, or every 100th Write. */
    UCHAR prefix[3];
    ULONG prefixLength = 0;
    BOOLEAN select = FALSE;
    if (First && m_portSelect) {
        select = (Port != m_lastPort) || (m_f5Countdown <= 1);
        if (select) {
            prefix[prefixLength++] = 0xF5;
            prefix[prefixLength++] = (UCHAR)(Port + 1);
            if (Data[0] < 0x80 && m_running[Port] != 0) {
                prefix[prefixLength++] = m_running[Port];   /* data continues a message */
            }
        }
    }

    ULONG room = SW_MIDI_TX_BYTES - 1 - Queued();
    if (prefixLength + Length > room) {
        return 0;
    }

    if (First && m_portSelect) {
        if (select) {
            m_lastPort = Port;
            m_f5Countdown = SW_F5_RESEND_COUNT;
        } else {
            m_f5Countdown--;
        }
    }
    for (ULONG i = 0; i < prefixLength; ++i) {
        Push(prefix[i]);
    }
    for (ULONG i = 0; i < Length; ++i) {
        UCHAR b = Data[i];
        if (b >= 0x80 && b < 0xF0) {
            m_running[Port] = b;                    /* channel status */
        } else if (b >= 0xF0 && b <= 0xF7) {
            m_running[Port] = 0;                    /* system common/exclusive cancels it */
        }                                           /* real-time (>= 0xF8) leaves it */
        Push(b);
    }
    if (Queued() > m_highWater) {
        m_highWater = Queued();
    }
    return Length;
}
