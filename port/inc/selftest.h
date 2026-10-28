/*
 * selftest.h - hardware self-tests run as diagnostic stages (diag.h).
 *
 * They exist for the hardware test rounds (port/TESTING.md) and run only when
 * DiagMaxStage asks for them. They are polled and run at PASSIVE_LEVEL during
 * StartDevice. They return failure only for a fault in the card's response
 * (a UART that never becomes ready); an absent cable or a silent synth is a
 * result to report, not a failed start.
 */

#ifndef SW1000_SELFTEST_H
#define SW1000_SELFTEST_H

#include "sw1000_uart.h"
#include "diag.h"

/* Stage 7: send a pattern on UART 0 and expect each byte back through a
 * MIDI OUT -> MIDI IN loopback cable. */
NTSTATUS SwMidiLoopbackTest(_In_ CSw1000Uart *Uart, _In_ CSwDiag *Diag);

/* Stage 8: wait until the XG engine's boot window after the DSP reset has
 * passed (logging anything it sends on UART 1 meanwhile), then send XG System
 * On and three notes to the synth on UART 1. ResetTime100ns is SwNow100ns()
 * units. */
NTSTATUS SwSynthTest(_In_ CSw1000Uart *Uart, _In_ CSwDiag *Diag, _In_ LONGLONG ResetTime100ns);

#endif /* SW1000_SELFTEST_H */
