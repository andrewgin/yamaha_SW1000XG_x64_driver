/*
 * diag.h - bring-up diagnostics for the hardware test rounds.
 *
 * The test machine has no kernel debugger, so the driver records what each
 * start did in the registry, under <service key>\Parameters, where
 * install/Run-HwTest.ps1 collects it (port/TESTING.md):
 *
 *   DiagMaxStage      DWORD   set by the test script: run stages 1..N. Absent:
 *                             SW_DIAG_DEFAULT_MAX_STAGE (identify only).
 *   DiagLog           REG_SZ  this start's log, rewritten at every stage edge
 *   DiagBreadcrumb    DWORD   stage in progress, flushed to disk before the
 *                             stage runs and deleted after it
 *   DiagCrashedStage  DWORD   a start found a breadcrumb: the previous attempt
 *                             never finished that stage (crash, hang or power
 *                             loss). While it exists the driver runs no stage
 *                             that touches the card; the test script clears it.
 *   DiagCrashLog      REG_SZ  the log of that unfinished attempt
 *   DiagStartCount    DWORD   starts so far
 *   DiagStageCount    DWORD   stages this build knows
 *   DiagSelfTest      DWORD   set by the test script: run the hardware
 *                             self-tests (selftest.h) during this start. The
 *                             driver deletes it on reading, so a later boot
 *                             brings the card up without test notes or waits.
 *
 * Diagnostics never block a start: registry failures only lose the log.
 * PASSIVE_LEVEL only (StartDevice).
 */

#ifndef SW1000_DIAG_H
#define SW1000_DIAG_H

#include "common.h"

/* Bring-up stages, in order. Hardware access begins at SW_STAGE_MAP. */
typedef enum _SW_STAGE {
    SW_STAGE_NONE = 0,
    SW_STAGE_IDENTIFY,      /* log build, IDs and resources; no hardware access */
    SW_STAGE_MAP,           /* map the register BAR */
    SW_STAGE_PROBE,         /* read-only register probe */
    SW_STAGE_MICROCODE,     /* load + verify the registry bundle */
    SW_STAGE_COLDSTART,     /* DSP cold start (M2) + read-back checks */
    SW_STAGE_UART,          /* 8251 init + TX/RX enable (M7); self-tests: MIDI
                               loopback, polled notes to the XG synth */
    SW_STAGE_IRQ,           /* connect the interrupt (M3); self-test: notes
                               through the interrupt-driven UART 1 path */
    SW_STAGE_MIDIPORTS,     /* register the MIDI subdevices: SWXG1-3, MIDIOUT, MIDIIN (M7) */
    SW_STAGE_END
} SW_STAGE;

#define SW_STAGE_LAST              ((ULONG)SW_STAGE_END - 1)

/* Development builds touch nothing unless the test script asks. Release builds
 * will default to SW_STAGE_LAST once bring-up is proven (M9). */
#define SW_DIAG_DEFAULT_MAX_STAGE  ((ULONG)SW_STAGE_IDENTIFY)

#define SW_DIAG_LOG_BYTES          16384u

class CSwDiag
{
public:
    CSwDiag() = default;
    ~CSwDiag();

    /* Read the settings, detect an unfinished previous start, write the header. */
    void StartSession();
    /* Final result line; persist the log; close the key. */
    void EndSession(_In_ NTSTATUS status);

    /* TRUE: run the stage (breadcrumb is on disk). FALSE: stop here and return
     * StopStatus() - success when stopped by DiagMaxStage, failure when a
     * crash marker blocks hardware access. */
    BOOLEAN  BeginStage(_In_ SW_STAGE stage);
    void     EndStage(_In_ NTSTATUS status);
    NTSTATUS StopStatus() const { return m_stopStatus; }
    BOOLEAN  SelfTest() const { return m_selfTest; }

    void Log(_In_z_ _Printf_format_string_ const char *format, ...);

    static const char *StageName(_In_ ULONG stage);

private:
    BOOLEAN ReadDword(_In_z_ PCWSTR name, _Out_ PULONG value);
    void    WriteDword(_In_z_ PCWSTR name, _In_ ULONG value);
    void    DeleteValue(_In_z_ PCWSTR name);
    void    CopyValue(_In_z_ PCWSTR from, _In_z_ PCWSTR to);
    void    PersistLog();

    HANDLE   m_key = nullptr;
    ULONG    m_maxStage = SW_DIAG_DEFAULT_MAX_STAGE;
    ULONG    m_crashedStage = 0;
    SW_STAGE m_stage = SW_STAGE_NONE;          /* in progress */
    ULONG    m_lastCompleted = 0;
    NTSTATUS m_stopStatus = STATUS_SUCCESS;
    LONGLONG m_t0 = 0;
    LONGLONG m_stageT0 = 0;
    ULONG    m_used = 0;
    BOOLEAN  m_truncated = FALSE;
    BOOLEAN  m_selfTest = FALSE;
    char     m_log[SW_DIAG_LOG_BYTES];
};

#endif /* SW1000_DIAG_H */
