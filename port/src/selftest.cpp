/*
 * selftest.cpp - MIDI self-tests for the hardware test rounds (selftest.h).
 *
 * Message choices:
 *   loopback  Note On, channel 16, velocity 0 (a Note Off): harmless if the
 *             cable goes to a real instrument instead of back to MIDI IN.
 *   synth     F5 01 selects the synth's first port group, as the original
 *             does before its first message on UART 1 (re/10). XG System On
 *             (public XG spec) puts the synth in a known state; then C4, E4,
 *             G4 on channel 1 (default voice: piano).
 *
 * Clean-room; written from the notes.
 */

#include "selftest.h"
#include "sw1000_dsp.h"             /* SW_H8_READY_100NS */

#define SW_UART_BYTE_TIMEOUT_US  5000u      /* one MIDI byte takes 320 us */
#define SW_LOOPBACK_MESSAGES     16u

#pragma code_seg("PAGE")

static void
SleepMs(_In_ ULONG ms)
{
    PAGED_CODE();
    LARGE_INTEGER delay;
    delay.QuadPart = -(LONGLONG)ms * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &delay);
}

static NTSTATUS
SendBytes(_In_ CSw1000Uart *Uart, _In_ ULONG Ch, _In_reads_(Count) const UCHAR *Bytes, _In_ ULONG Count)
{
    PAGED_CODE();
    for (ULONG i = 0; i < Count; ++i) {
        NTSTATUS status = Uart->WriteBytePolled(Ch, Bytes[i], SW_UART_BYTE_TIMEOUT_US);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS
SwMidiLoopbackTest(_In_ CSw1000Uart *Uart, _In_ CSwDiag *Diag)
{
    PAGED_CODE();

    /* Anything already waiting in the receiver predates the test. */
    UCHAR b = 0;
    for (ULONG stale = 0; stale < 16 && Uart->ReadBytePolled(0, &b, 0) != STATUS_IO_TIMEOUT; ++stale) {
        Diag->Log("  stale byte 0x%02x in the receiver before the test", (ULONG)b);
    }

    /* One byte at a time: the 8251 receiver holds a single byte, so each echo
     * is read before the next byte goes out. */
    ULONG sent = 0, received = 0, mismatched = 0, errors = 0, maxUs = 0, sumUs = 0;
    for (ULONG i = 0; i < SW_LOOPBACK_MESSAGES * 3; ++i) {
        UCHAR tx = (i % 3 == 0) ? 0x9F : (i % 3 == 1) ? (UCHAR)(0x30 + i / 3) : 0x00;
        ULONG64 t0 = SwNow100ns();
        NTSTATUS status = Uart->WriteBytePolled(0, tx, SW_UART_BYTE_TIMEOUT_US);
        if (!NT_SUCCESS(status)) {
            Diag->Log("  UART0 never became ready to send byte %lu (status 0x%02x)",
                      i, (ULONG)Uart->Status(0));
            return status;
        }
        sent++;

        UCHAR rx = 0;
        status = Uart->ReadBytePolled(0, &rx, SW_UART_BYTE_TIMEOUT_US);
        if (status == STATUS_IO_TIMEOUT) {
            if (received == 0) {
                Diag->Log("  nothing came back after the first byte: is the cable connected MIDI OUT -> MIDI IN?");
                break;
            }
            Diag->Log("  byte %lu (0x%02x) did not come back", i, (ULONG)tx);
            continue;
        }
        ULONG us = (ULONG)((SwNow100ns() - t0) / 10);
        received++;
        sumUs += us;
        if (us > maxUs) {
            maxUs = us;
        }
        if (status == STATUS_DATA_ERROR) {
            errors++;
        }
        if (rx != tx) {
            if (mismatched < 4) {
                Diag->Log("  byte %lu: sent 0x%02x, got 0x%02x", i, (ULONG)tx, (ULONG)rx);
            }
            mismatched++;
        }
    }

    Diag->Log("  loopback: sent %lu, received %lu, mismatched %lu, receive errors %lu; "
              "round trip max %lu us, average %lu us",
              sent, received, mismatched, errors, maxUs, received ? sumUs / received : 0);
    Diag->Log("  loopback result: %s",
              received == 0 ? "NO ECHO" :
              (received == sent && mismatched == 0 && errors == 0) ? "PASS" : "PARTIAL");
    return STATUS_SUCCESS;
}

NTSTATUS
SwSynthTest(_In_ CSw1000Uart *Uart, _In_ CSwDiag *Diag, _In_ LONGLONG ResetTime100ns)
{
    PAGED_CODE();

    /*
     * The original waits 10 s after the DSP reset before its first message to
     * the synth (WaitH8, re/05). Meanwhile, listen on UART 1: the original
     * never enables that receiver, so whether the synth's controller sends
     * anything while it boots is unknown. Diagnostic only; disabled again.
     */
    LONGLONG readyAt = ResetTime100ns + SW_H8_READY_100NS;
    Diag->Log("  %lu ms since the DSP reset; waiting until 10000 ms for the XG engine",
              (ULONG)(((LONGLONG)SwNow100ns() - ResetTime100ns) / 10000));
    Uart->EnableRx(1);
    ULONG heard = 0;
    while ((LONGLONG)SwNow100ns() < readyAt) {
        UCHAR b = 0;
        NTSTATUS status = Uart->ReadBytePolled(1, &b, 0);
        if (status == STATUS_IO_TIMEOUT) {
            SleepMs(1);
            continue;
        }
        if (heard < 32) {
            Diag->Log("  UART1 received 0x%02x at %lu ms after reset%s", (ULONG)b,
                      (ULONG)(((LONGLONG)SwNow100ns() - ResetTime100ns) / 10000),
                      status == STATUS_DATA_ERROR ? " (error flag set)" : "");
        }
        heard++;
    }
    Uart->DisableRx(1);
    Diag->Log("  UART1 received %lu bytes during the boot window", heard);

    static const UCHAR kXgOn[] = { 0xF5, 0x01, 0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7 };
    NTSTATUS status = SendBytes(Uart, 1, kXgOn, (ULONG)sizeof(kXgOn));
    Diag->Log("  sent F5 01 + XG System On: 0x%08x (UART1 status 0x%02x)", status, (ULONG)Uart->Status(1));
    if (!NT_SUCCESS(status)) {
        return status;
    }
    SleepMs(200);

    static const UCHAR kNotes[] = { 0x3C, 0x40, 0x43 };            /* C4, E4, G4 */
    for (ULONG i = 0; i < ARRAYSIZE(kNotes); ++i) {
        const UCHAR on[]  = { 0x90, kNotes[i], 0x64 };
        const UCHAR off[] = { 0x80, kNotes[i], 0x40 };
        status = SendBytes(Uart, 1, on, (ULONG)sizeof(on));
        if (NT_SUCCESS(status)) {
            SleepMs(i == ARRAYSIZE(kNotes) - 1 ? 900 : 450);
            status = SendBytes(Uart, 1, off, (ULONG)sizeof(off));
        }
        if (!NT_SUCCESS(status)) {
            Diag->Log("  note %lu: send failed 0x%08x", i, status);
            return status;
        }
        SleepMs(100);
    }
    Diag->Log("  played C4, E4, G4 on channel 1 (about 2 s): the tester reports whether it was heard");
    return STATUS_SUCCESS;
}

#pragma code_seg()
