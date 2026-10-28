/*
 * sw1000_miditx.h - MIDI transmit queue and receive ring (M7, re/10).
 *
 * Producer: the MIDI miniports' Write, through IInterruptSync::
 * CallSynchronizedRoutine. Consumer: the ISR, one byte per UART TX-ready
 * interrupt. Every method therefore runs under the interrupt-sync lock, at
 * DIRQL, on non-paged data.
 *
 * Port select (UART 1, XG synth; off for UART 0's MIDI OUT socket): before
 * the data of a Write the original
 * sends F5 <port+1> when the port group differs from the channel's last one,
 * or every 100 messages, and then re-sends the port's running status if the
 * data starts without a status byte (re/10 "Output path"). The same rule
 * applies here, per Write call.
 *
 * Unlike the original's 8 KB FIFO, which made Write busy-wait up to 5.2 s
 * when full (H-023), a full queue makes Enqueue take nothing: Write then
 * returns a short count and the MIDI port driver retries.
 *
 * Clean-room; written from the notes.
 */

#ifndef SW1000_MIDITX_H
#define SW1000_MIDITX_H

#include "common.h"

#define SW_MIDI_TX_BYTES        4096u       /* power of two */
#define SW_F5_RESEND_COUNT      100u
#define SW_MIDI_PORT_GROUPS     3u          /* SWXG1-3 */

class CSw1000MidiTx
{
public:
    /* PortSelect: TRUE for UART 1 (F5 prefixes, Port = 0-2); FALSE for UART 0,
     * whose bytes go out unchanged (Port must be 0). */
    void Reset(_In_ BOOLEAN PortSelect);

    /* Queue Length bytes for port group Port, with the F5 prefix when needed
     * (First: this chunk starts a Write call). All or nothing: returns Length
     * when queued, 0 when there isn't room. */
    ULONG Enqueue(_In_ ULONG Port, _In_reads_(Length) const UCHAR *Data,
                  _In_ ULONG Length, _In_ BOOLEAN First);

    BOOLEAN IsEmpty() const { return m_head == m_tail; }
    UCHAR   Front() const   { return m_buf[m_tail]; }
    void    Pop()           { m_tail = (m_tail + 1) & (SW_MIDI_TX_BYTES - 1); }
    ULONG   Queued() const  { return (m_head - m_tail) & (SW_MIDI_TX_BYTES - 1); }
    ULONG   HighWater() const { return m_highWater; }

private:
    void Push(_In_ UCHAR value)
    {
        m_buf[m_head] = value;
        m_head = (m_head + 1) & (SW_MIDI_TX_BYTES - 1);
    }

    UCHAR          m_buf[SW_MIDI_TX_BYTES];
    volatile ULONG m_head = 0;                          /* next free slot */
    volatile ULONG m_tail = 0;                          /* next byte to send */
    ULONG          m_highWater = 0;
    BOOLEAN        m_portSelect = TRUE;
    ULONG          m_lastPort = MAXULONG;               /* none selected yet */
    ULONG          m_f5Countdown = SW_F5_RESEND_COUNT;
    UCHAR          m_running[SW_MIDI_PORT_GROUPS] = {}; /* running status, 0 = none */
};

/*
 * MIDI IN receive ring (UART 0). Producer: the ISR; consumer: the capture
 * stream's Read through CallSynchronizedRoutine. 1 KB, as the original;
 * bytes arriving when it is full are dropped and counted (re/10).
 */
#define SW_MIDI_RX_BYTES        1024u       /* power of two */

class CSw1000MidiRx
{
public:
    void Clear() { m_head = 0; m_tail = 0; }
    BOOLEAN Push(_In_ UCHAR value)
    {
        ULONG next = (m_head + 1) & (SW_MIDI_RX_BYTES - 1);
        if (next == m_tail) {
            m_dropped++;
            return FALSE;
        }
        m_buf[m_head] = value;
        m_head = next;
        return TRUE;
    }
    ULONG Pop(_Out_writes_to_(Max, return) PUCHAR Out, _In_ ULONG Max)
    {
        ULONG n = 0;
        while (n < Max && m_tail != m_head) {
            Out[n++] = m_buf[m_tail];
            m_tail = (m_tail + 1) & (SW_MIDI_RX_BYTES - 1);
        }
        return n;
    }
    ULONG Dropped() const { return m_dropped; }

private:
    UCHAR          m_buf[SW_MIDI_RX_BYTES];
    volatile ULONG m_head = 0;
    volatile ULONG m_tail = 0;
    ULONG          m_dropped = 0;
};

#endif /* SW1000_MIDITX_H */
