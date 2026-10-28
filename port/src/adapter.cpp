/*
 * adapter.cpp - the SW1000XG adapter object.
 *
 * The COM object PortCls talks to and the owner of the hardware: it validates
 * the PnP resources, maps the BAR once, brings up the DSP (M2) and the UARTs,
 * owns the interrupt (M3, UART sources so far) and the synth's MIDI transmit
 * queue (M7). Wave channels, mixer and power arrive in later milestones.
 *
 * Clean-room: no Yamaha code; scaffolding follows the public WDK PortCls samples.
 */

/* portcls.h includes initguid.h when PUT_GUIDS_HERE is defined, so the GUIDs
 * (PortCls's and our IID_ISw1000Adapter) are instantiated in this one file. */
#define PUT_GUIDS_HERE
#include "adapter.h"
#include "sw1000_hw.h"
#include "sw1000_dsp.h"
#include "sw1000_uart.h"
#include "sw1000_miditx.h"
#include "selftest.h"

/*
 * The adapter class. It implements our private ISw1000Adapter and the PortCls
 * IAdapterPowerManagement, over the stdunk CUnknown helper (same shape as the
 * DDK msvad CAdapterCommon).
 */
class CSw1000Adapter : public ISw1000Adapter,
                       public IAdapterPowerManagement,
                       public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CSw1000Adapter);
    ~CSw1000Adapter();

    /* ISw1000Adapter */
    STDMETHODIMP_(NTSTATUS) Init(
        _In_ PRESOURCELIST ResourceList,
        _In_ PDEVICE_OBJECT DeviceObject,
        _In_ CSwDiag *Diag);
    STDMETHODIMP_(NTSTATUS) MidiWrite(
        _In_ ULONG Port,
        _In_reads_bytes_(Length) PVOID Data,
        _In_ ULONG Length,
        _Out_ PULONG Written);
    STDMETHODIMP_(NTSTATUS) MidiInStart(_In_ PPORTMIDI Port, _In_ PSERVICEGROUP ServiceGroup);
    STDMETHODIMP_(void)     MidiInStop();
    STDMETHODIMP_(NTSTATUS) MidiRead(
        _Out_writes_bytes_to_(Length, *Read) PVOID Buffer,
        _In_ ULONG Length,
        _Out_ PULONG Read);

    /* IAdapterPowerManagement - stubbed until later milestone. */
    STDMETHODIMP_(void)     PowerChangeState(_In_ POWER_STATE NewState);
    STDMETHODIMP_(NTSTATUS) QueryPowerChangeState(_In_ POWER_STATE NewStateQuery);
    STDMETHODIMP_(NTSTATUS) QueryDeviceCapabilities(_In_ PDEVICE_CAPABILITIES Caps);

private:
    void LogRegisters(_In_ CSwDiag *Diag);
    void LogStreamRegisters(_In_ CSwDiag *Diag);
    NTSTATUS ConnectInterrupt(_In_ PRESOURCELIST ResourceList, _In_ CSwDiag *Diag);
    NTSTATUS InterruptSelfTest(_In_ CSwDiag *Diag);

    /* DIRQL, under the interrupt-sync lock */
    static NTSTATUS Isr(_In_ PINTERRUPTSYNC InterruptSync, _In_ PVOID Context);
    static NTSTATUS EnqueueSynced(_In_ PINTERRUPTSYNC InterruptSync, _In_ PVOID Context);
    static NTSTATUS DisableSourcesSynced(_In_ PINTERRUPTSYNC InterruptSync, _In_ PVOID Context);
    static NTSTATUS MidiInStartSynced(_In_ PINTERRUPTSYNC InterruptSync, _In_ PVOID Context);
    static NTSTATUS MidiInStopSynced(_In_ PINTERRUPTSYNC InterruptSync, _In_ PVOID Context);
    static NTSTATUS MidiReadSynced(_In_ PINTERRUPTSYNC InterruptSync, _In_ PVOID Context);
    NTSTATUS ServiceInterrupt();
    void     ServiceTx(_In_ ULONG Channel, _Inout_ CSw1000MidiTx &Queue, _In_ ULONG Bit);
    void     ServiceMidiIn();
    void     DisableSourceIsr(_In_ ULONG Bit, _In_ BOOLEAN Fault)
    {
        m_hw.ModifyTrpifEnableSynced(Bit, 0);
        if (Fault) {
            m_faultMask |= Bit;
        }
    }

    PDEVICE_OBJECT   m_DeviceObject = nullptr;
    CSw1000Hw        m_hw;                       /* BAR + registers + host port */
    CSw1000Uart      m_uart;                     /* the two MIDI UARTs (M7) */
    CSw1000Microcode m_microcode;               /* install-provided DSP tables */
    LONGLONG         m_resetTime100ns = 0;       /* DSP reset time, for WaitH8 (M6) */
    BOOLEAN          m_hwTouched = FALSE;        /* DSP programming has begun */

    PINTERRUPTSYNC   m_interruptSync = nullptr;  /* M3 */
    CSw1000MidiTx    m_synthTx;                  /* UART 1 queue (SWXG1-3) */
    CSw1000MidiTx    m_extTx;                    /* UART 0 queue (MIDI OUT socket) */
    CSw1000MidiRx    m_midiIn;                   /* UART 0 receive ring (MIDI IN) */
    PPORTMIDI        m_midiInPort = nullptr;     /* set while the capture stream runs */
    PSERVICEGROUP    m_midiInGroup = nullptr;

    /* ISR statistics and guards (written only under the interrupt lock) */
    ULONG            m_isrCalls = 0;             /* interrupts that were ours */
    ULONG            m_isrNotOurs = 0;           /* shared-line calls for others */
    ULONG            m_txBytes[SW_UART_CHANNELS] = {};
    ULONG            m_notReadyRun[SW_UART_CHANNELS + 1] = {};  /* TX 0, TX 1, RX 0 */
    ULONG            m_rxBytes = 0;
    ULONG            m_rxErrors = 0;
    ULONG            m_faultMask = 0;            /* sources turned off by the guard */
};

/* A source that keeps interrupting while the UART says it isn't ready this many
 * times in a row means the status bit doesn't track the UART: turn the source
 * off rather than let a level-triggered line storm. */
#define SW_NOT_READY_LIMIT      1000u
#define SW_RX_BURST_MAX         16u         /* bytes drained per RX interrupt */

/* ------------------------------------------------------------------------- */

CSw1000Adapter::~CSw1000Adapter()
{
    PAGED_CODE();
    /*
     * Interrupt sources off (under the interrupt lock) BEFORE disconnecting:
     * the line is shared and level-triggered, so a source left asserting with
     * no handler would storm.
     */
    if (m_interruptSync != nullptr) {
        m_interruptSync->CallSynchronizedRoutine(DisableSourcesSynced, this);
        m_interruptSync->Disconnect();
        m_interruptSync->Release();
        m_interruptSync = nullptr;
    }
    /*
     * Leave the card quiet before the BAR goes away (failed start after the
     * cold start, or device removal): sources off, IRQEN/RUN cleared, muted.
     * Skipped if we never programmed the DSP. m_hw unmaps the BAR afterwards
     * in its own destructor.
     */
    if (m_hwTouched) {
        m_hw.Quiesce();
    }
}

STDMETHODIMP_(NTSTATUS)
CSw1000Adapter::NonDelegatingQueryInterface(_In_ REFIID Interface, _COM_Outptr_ PVOID *Object)
{
    PAGED_CODE();

    if (Object == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }

    if (IsEqualGUIDAligned(Interface, IID_IUnknown)) {
        *Object = reinterpret_cast<PVOID>(static_cast<PUNKNOWN>(static_cast<ISw1000Adapter*>(this)));
    } else if (IsEqualGUIDAligned(Interface, IID_ISw1000Adapter)) {
        *Object = reinterpret_cast<PVOID>(static_cast<ISw1000Adapter*>(this));
    } else if (IsEqualGUIDAligned(Interface, IID_IAdapterPowerManagement)) {
        *Object = reinterpret_cast<PVOID>(static_cast<IAdapterPowerManagement*>(this));
    } else {
        *Object = nullptr;
        return STATUS_INVALID_PARAMETER;
    }

    static_cast<PUNKNOWN>(*Object)->AddRef();
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------- */

/*
 * Log every translated resource and return the first memory BAR. re/02: the
 * device needs one memory BAR and one interrupt. The original required exactly
 * one of each on its adapter sublist; we require at least one of each.
 */
static NTSTATUS
CheckResources(_In_ PRESOURCELIST List, _In_ CSwDiag *Diag,
               _Out_ PCM_PARTIAL_RESOURCE_DESCRIPTOR *Mem)
{
    PAGED_CODE();

    *Mem = nullptr;
    ULONG numMem = List->NumberOfEntriesOfType(CmResourceTypeMemory);
    ULONG numIrq = List->NumberOfEntriesOfType(CmResourceTypeInterrupt);
    Diag->Log("  resources: %lu in all, %lu memory, %lu interrupt",
              List->NumberOfEntries(), numMem, numIrq);

    for (ULONG i = 0; i < numMem; ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = List->FindTranslatedEntry(CmResourceTypeMemory, i);
        if (d != nullptr) {
            Diag->Log("  memory %lu: start 0x%llx length 0x%lx (%lu KB) flags 0x%x",
                      i, (ULONGLONG)d->u.Memory.Start.QuadPart, d->u.Memory.Length,
                      d->u.Memory.Length / 1024, d->Flags);
        }
    }
    for (ULONG i = 0; i < numIrq; ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = List->FindTranslatedEntry(CmResourceTypeInterrupt, i);
        if (d != nullptr) {
            Diag->Log("  interrupt %lu: %s, level %lu vector %lu affinity 0x%llx share %u",
                      i,
                      (d->Flags & CM_RESOURCE_INTERRUPT_MESSAGE) ? "message-signalled" :
                      (d->Flags & CM_RESOURCE_INTERRUPT_LATCHED) ? "edge" : "level",
                      (ULONG)d->u.Interrupt.Level, d->u.Interrupt.Vector,
                      (ULONGLONG)d->u.Interrupt.Affinity, (ULONG)d->ShareDisposition);
        }
    }

    if (numMem < 1 || numIrq < 1) {
        Diag->Log("  need at least one memory and one interrupt resource");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    *Mem = List->FindTranslatedEntry(CmResourceTypeMemory, 0);
    if (*Mem == nullptr) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if ((*Mem)->u.Memory.Length < SW_BAR_MIN_LENGTH) {
        Diag->Log("  memory 0 is smaller than the 0x%x bytes the register map spans (U-006)",
                  SW_BAR_MIN_LENGTH);
    }
    return STATUS_SUCCESS;
}

/* Read-only snapshot of the registers that can be read without side effects
 * (TRPIF's read semantics are themselves U-004). */
void
CSw1000Adapter::LogRegisters(_In_ CSwDiag *Diag)
{
    PAGED_CODE();

    Diag->Log("  PORT0 0x%08x (bits 11/10 = SO/SI option, active low; bit 19 = SI 4ch)",
              m_hw.ReadPort0());
    Diag->Log("  TRPIF status 0x%08x, MKS 0x%08x", m_hw.ReadTrpifStatus(), m_hw.ReadMks());
    Diag->Log("  UART0 status 0x%02x, UART1 status 0x%02x",
              (ULONG)m_hw.ReadUartStatus(0), (ULONG)m_hw.ReadUartStatus(1));
    Diag->Log("  host port CTRL 0-4: 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x",
              m_hw.ReadHostPortCtrl(0), m_hw.ReadHostPortCtrl(1), m_hw.ReadHostPortCtrl(2),
              m_hw.ReadHostPortCtrl(3), m_hw.ReadHostPortCtrl(4));

    /* Is the sample counter running, and at what rate? Two reads about 50 ms
     * apart, timed at QPC resolution (round 1 used the 15.6 ms tick). */
    ULONG scr0 = m_hw.ReadScr();
    ULONG64 t0 = SwNow100ns();
    LARGE_INTEGER delay;
    delay.QuadPart = -500000LL;                     /* 50 ms, relative */
    KeDelayExecutionThread(KernelMode, FALSE, &delay);
    ULONG scr1 = m_hw.ReadScr();
    ULONG64 elapsed = SwNow100ns() - t0;
    ULONG delta = scr1 - scr0;
    Diag->Log("  SCR 0x%08x -> 0x%08x over %lu us (delta %lu, about %lu per second)",
              scr0, scr1, (ULONG)(elapsed / 10), delta,
              elapsed ? (ULONG)((ULONG64)delta * 10000000ULL / elapsed) : 0);
}

/* After the cold start: stream-engine registers through the RAM read path the
 * original also uses (GetTRWFO, GetSUSF, GetTP), plus host-port timing. */
void
CSw1000Adapter::LogStreamRegisters(_In_ CSwDiag *Diag)
{
    PAGED_CODE();

    static const struct { ULONG addr; const char *name; } kRegs[] = {
        { SW_RAM_TRWF,    "TRWF" },
        { SW_RAM_TRWFO,   "TRWFO" },
        { SW_RAM_SUSF,    "SUSF" },
        { SW_RAM_TP_BASE, "TP[0]" },
    };
    for (ULONG i = 0; i < ARRAYSIZE(kRegs); ++i) {
        ULONG value = 0;
        NTSTATUS status = m_hw.RamReadWord(kRegs[i].addr, &value);
        Diag->Log("  RAM 0x%04lx %-5s = 0x%08x (0x%08x)", kRegs[i].addr, kRegs[i].name, value, status);
    }
    Diag->Log("  host port: longest BUSY wait %lu us, %lu timeouts (limit %lu us)",
              m_hw.MaxBusyWaitUs(), m_hw.BusyTimeouts(), SW_HOSTPORT_TIMEOUT_US);
}

/*
 * Bring-up, one diagnostics stage at a time (diag.h). Each stage stops here
 * when DiagMaxStage says so; StartDevice then succeeds with the card left as
 * the last completed stage put it.
 */
STDMETHODIMP_(NTSTATUS)
CSw1000Adapter::Init(_In_ PRESOURCELIST ResourceList, _In_ PDEVICE_OBJECT DeviceObject,
                     _In_ CSwDiag *Diag)
{
    PAGED_CODE();

    ASSERT(ResourceList != nullptr);
    ASSERT(DeviceObject != nullptr);
    ASSERT(Diag != nullptr);

    m_DeviceObject = DeviceObject;

    /* Stage 1: resources (no hardware access). */
    if (!Diag->BeginStage(SW_STAGE_IDENTIFY)) {
        return Diag->StopStatus();
    }
    PCM_PARTIAL_RESOURCE_DESCRIPTOR mem = nullptr;
    NTSTATUS status = CheckResources(ResourceList, Diag, &mem);
    Diag->EndStage(status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /*
     * Stage 2: map the BAR ONCE (H-003; the original mapped it twice, here and
     * again for the UART). H-002/H-011: MapBar checks the result and the
     * minimum length. Non-cached MMIO (re/02, re/04).
     */
    if (!Diag->BeginStage(SW_STAGE_MAP)) {
        return Diag->StopStatus();
    }
    status = m_hw.MapBar(mem->u.Memory.Start, mem->u.Memory.Length);
    Diag->EndStage(status);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    m_uart.Attach(&m_hw);

    /* Stage 3: read-only probe of the untouched card (U-005, U-006). */
    if (!Diag->BeginStage(SW_STAGE_PROBE)) {
        return Diag->StopStatus();
    }
    LogRegisters(Diag);
    Diag->EndStage(STATUS_SUCCESS);

    /*
     * Stage 4: the DSP microcode. H-009: the bundle is the installer-provided
     * tables extracted from the user's own yswds.sys; if it is absent or any
     * hash fails, refuse to start.
     */
    if (!Diag->BeginStage(SW_STAGE_MICROCODE)) {
        return Diag->StopStatus();
    }
    status = m_microcode.Load();
    if (!NT_SUCCESS(status)) {
        Diag->Log("  microcode bundle missing or invalid: run Install-Microcode.ps1");
    }
    Diag->EndStage(status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /*
     * Stage 5 (M2): DSP cold start. H-017: a failed download fails StartDevice
     * (the original ignored it). Afterwards the DSP runs with IRQEN/RUN set and
     * TRPIF still 0; the interrupt is connected in M3.
     */
    if (!Diag->BeginStage(SW_STAGE_COLDSTART)) {
        return Diag->StopStatus();
    }
    CSw1000Dsp dsp(&m_hw, &m_microcode, Diag);
    m_hwTouched = TRUE;
    status = dsp.ColdStart();
    if (NT_SUCCESS(status)) {
        m_resetTime100ns = dsp.ResetTime100ns();
        LogRegisters(Diag);
        LogStreamRegisters(Diag);
    }
    Diag->EndStage(status);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    /* The microcode bundle stays loaded for the life of the adapter (D0
     * re-init will reuse it). */

    /*
     * Stage 6 (M7): the UARTs, as the original's start-up does them (re/05):
     * 8251 init on both channels, TX on both, RX on channel 0. No TRPIF bits
     * are enabled yet. Self-tests (one start only, DiagSelfTest): the MIDI
     * loopback on UART 0 and polled notes to the synth on UART 1.
     */
    if (!Diag->BeginStage(SW_STAGE_UART)) {
        return Diag->StopStatus();
    }
    for (ULONG ch = 0; ch < SW_UART_CHANNELS; ++ch) {
        m_uart.Init(ch);
        m_uart.EnableTx(ch);
    }
    m_uart.EnableRx(0);
    Diag->Log("  command UART0 0x%02x, UART1 0x%02x; status UART0 0x%02x, UART1 0x%02x",
              (ULONG)m_uart.Command(0), (ULONG)m_uart.Command(1),
              (ULONG)m_hw.ReadUartStatus(0), (ULONG)m_hw.ReadUartStatus(1));
    status = STATUS_SUCCESS;
    if (Diag->SelfTest()) {
        status = SwMidiLoopbackTest(&m_uart, Diag);
        if (NT_SUCCESS(status)) {
            status = SwSynthTest(&m_uart, Diag, m_resetTime100ns);
        }
    }
    Diag->EndStage(status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /*
     * Stage 7 (M3): connect the interrupt. From here UART 1 transmits only
     * through m_synthTx and the ISR. Self-test: notes through that path.
     */
    if (!Diag->BeginStage(SW_STAGE_IRQ)) {
        return Diag->StopStatus();
    }
    status = ConnectInterrupt(ResourceList, Diag);
    if (NT_SUCCESS(status) && Diag->SelfTest()) {
        status = InterruptSelfTest(Diag);
    }
    Diag->EndStage(status);

    /* Stage 8 (the SWXG MIDI subdevices) runs in StartDevice, which owns the
     * port drivers (driver.cpp). */
    return status;
}

/* ---- interrupt (M3) and the synth transmit path (M7) ---------------------- */

NTSTATUS
CSw1000Adapter::ConnectInterrupt(_In_ PRESOURCELIST ResourceList, _In_ CSwDiag *Diag)
{
    PAGED_CODE();

    m_synthTx.Reset(TRUE);
    m_extTx.Reset(FALSE);
    m_midiIn.Clear();
    NTSTATUS status = PcNewInterruptSync(&m_interruptSync, nullptr, ResourceList, 0,
                                         InterruptSyncModeNormal);
    if (NT_SUCCESS(status)) {
        status = m_interruptSync->RegisterServiceRoutine(Isr, this, FALSE);
    }
    if (NT_SUCCESS(status)) {
        status = m_interruptSync->Connect();
    }
    Diag->Log("  interrupt sync: create/register/connect 0x%08x", status);
    if (!NT_SUCCESS(status) && m_interruptSync != nullptr) {
        m_interruptSync->Release();
        m_interruptSync = nullptr;
    }
    return status;
}

/* Notes G4, E4, C4 on SWXG1 through MidiWrite: the path the MIDI ports use. */
NTSTATUS
CSw1000Adapter::InterruptSelfTest(_In_ CSwDiag *Diag)
{
    PAGED_CODE();

    static const UCHAR kNotes[] = { 0x43, 0x40, 0x3C };
    LARGE_INTEGER delay;
    for (ULONG i = 0; i < ARRAYSIZE(kNotes); ++i) {
        UCHAR on[]  = { 0x90, kNotes[i], 0x64 };
        UCHAR off[] = { 0x80, kNotes[i], 0x40 };
        ULONG written = 0;
        MidiWrite(0, on, (ULONG)sizeof(on), &written);
        delay.QuadPart = (i == ARRAYSIZE(kNotes) - 1) ? -9000000LL : -4500000LL;
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
        MidiWrite(0, off, (ULONG)sizeof(off), &written);
        delay.QuadPart = -1000000LL;
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }

    /* The queue should be empty within a few ms; give it a second. */
    for (ULONG ms = 0; ms < 1000 && !m_synthTx.IsEmpty(); ms += 10) {
        delay.QuadPart = -100000LL;
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }
    BOOLEAN drained = m_synthTx.IsEmpty();
    BOOLEAN fault = (m_faultMask & SW_TRPIF_UART1_TX) != 0;
    Diag->Log("  interrupt-driven notes G4, E4, C4: queue %s; ISR calls %lu (not ours %lu), "
              "bytes sent %lu, queue high water %lu%s",
              drained ? "drained" : "NOT drained", m_isrCalls, m_isrNotOurs, m_txBytes[1],
              m_synthTx.HighWater(), fault ? "; TX source turned off by the not-ready guard" : "");
    return (drained && !fault) ? STATUS_SUCCESS : STATUS_IO_TIMEOUT;
}

/* PortCls calls this for every interrupt on the (shared) line. */
NTSTATUS
CSw1000Adapter::Isr(_In_ PINTERRUPTSYNC InterruptSync, _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(InterruptSync);
    return static_cast<CSw1000Adapter *>(Context)->ServiceInterrupt();
}

NTSTATUS
CSw1000Adapter::ServiceInterrupt()
{
    /* The status read shows sources whether enabled or not (round 2), so only
     * enabled ones count. Nothing enabled: not ours, without touching TRPIF. */
    ULONG enabled = m_hw.TrpifShadow();
    if (enabled == 0) {
        m_isrNotOurs++;
        return STATUS_UNSUCCESSFUL;
    }
    ULONG pending = m_hw.ReadTrpifStatus() & enabled;
    if (pending == 0) {
        m_isrNotOurs++;
        return STATUS_UNSUCCESSFUL;
    }
    m_isrCalls++;

    if (pending & SW_TRPIF_UART1_TX) {
        ServiceTx(1, m_synthTx, SW_TRPIF_UART1_TX);
    }
    if (pending & SW_TRPIF_UART0_TX) {
        ServiceTx(0, m_extTx, SW_TRPIF_UART0_TX);
    }
    if (pending & SW_TRPIF_UART0_RX) {
        ServiceMidiIn();
    }
    /* Nothing else is ever enabled; if it were, switch it off rather than
     * leave a level-triggered source unserviced. */
    ULONG other = pending & ~(SW_TRPIF_UART1_TX | SW_TRPIF_UART0_TX | SW_TRPIF_UART0_RX);
    if (other != 0) {
        DisableSourceIsr(other, TRUE);
    }
    return STATUS_SUCCESS;
}

/* One byte per TX-ready interrupt, as the original (re/06); source off when
 * the queue is empty. */
void
CSw1000Adapter::ServiceTx(_In_ ULONG Channel, _Inout_ CSw1000MidiTx &Queue, _In_ ULONG Bit)
{
    if (Queue.IsEmpty()) {
        DisableSourceIsr(Bit, FALSE);
        return;
    }
    if (m_uart.StatusIsr(Channel) & SW_UART_ST_TXRDY) {
        m_uart.WriteDataIsr(Channel, Queue.Front());
        Queue.Pop();
        m_txBytes[Channel]++;
        m_notReadyRun[Channel] = 0;
    } else if (++m_notReadyRun[Channel] >= SW_NOT_READY_LIMIT) {
        DisableSourceIsr(Bit, TRUE);
    }
}

/* Drain the receiver (a burst at most), then wake the capture stream's Read
 * through the port driver, as the original's ISR does (re/06, re/10). */
void
CSw1000Adapter::ServiceMidiIn()
{
    ULONG got = 0;
    while (got < SW_RX_BURST_MAX) {
        UCHAR status = m_uart.StatusIsr(0);
        if ((status & SW_UART_ST_RXRDY) == 0) {
            break;
        }
        UCHAR value = m_uart.ReadDataIsr(0);
        if (status & SW_UART_ST_ERRORS) {
            m_uart.ErrorResetIsr(0);
            m_rxErrors++;
        }
        m_midiIn.Push(value);
        got++;
    }
    if (got != 0) {
        m_rxBytes += got;
        m_notReadyRun[SW_UART_CHANNELS] = 0;
        if (m_midiInPort != nullptr) {
            m_midiInPort->Notify(m_midiInGroup);
        }
    } else if (++m_notReadyRun[SW_UART_CHANNELS] >= SW_NOT_READY_LIMIT) {
        DisableSourceIsr(SW_TRPIF_UART0_RX, TRUE);
    }
}

typedef struct _SW_ENQUEUE_CONTEXT {
    CSw1000Adapter *Adapter;
    CSw1000MidiTx  *Queue;
    ULONG           Bit;                    /* the queue's TRPIF TX source */
    ULONG           Port;
    const UCHAR    *Data;
    ULONG           Length;
    BOOLEAN         First;
    ULONG           Queued;
} SW_ENQUEUE_CONTEXT;

NTSTATUS
CSw1000Adapter::EnqueueSynced(_In_ PINTERRUPTSYNC InterruptSync, _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(InterruptSync);
    SW_ENQUEUE_CONTEXT *c = static_cast<SW_ENQUEUE_CONTEXT *>(Context);
    c->Queued = c->Queue->Enqueue(c->Port, c->Data, c->Length, c->First);
    if (c->Queued != 0 && (c->Adapter->m_faultMask & c->Bit) == 0) {
        c->Adapter->m_hw.ModifyTrpifEnableSynced(0, c->Bit);    /* ISR takes it from here */
    }
    return STATUS_SUCCESS;
}

NTSTATUS
CSw1000Adapter::DisableSourcesSynced(_In_ PINTERRUPTSYNC InterruptSync, _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(InterruptSync);
    CSw1000Adapter *self = static_cast<CSw1000Adapter *>(Context);
    self->m_hw.ModifyTrpifEnableSynced(MAXULONG, 0);
    self->m_midiInPort = nullptr;
    self->m_midiInGroup = nullptr;
    return STATUS_SUCCESS;
}

typedef struct _SW_MIDIIN_CONTEXT {
    CSw1000Adapter *Adapter;
    PPORTMIDI       Port;
    PSERVICEGROUP   Group;
    PUCHAR          Buffer;                 /* MidiRead: stack chunk */
    ULONG           Length;
    ULONG           Done;
} SW_MIDIIN_CONTEXT;

/* Capture stream RUN: start clean (drop anything the receiver held while no
 * one listened), remember where to notify, turn the RX source on. */
NTSTATUS
CSw1000Adapter::MidiInStartSynced(_In_ PINTERRUPTSYNC InterruptSync, _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(InterruptSync);
    SW_MIDIIN_CONTEXT *c = static_cast<SW_MIDIIN_CONTEXT *>(Context);
    CSw1000Adapter *self = c->Adapter;
    self->m_midiIn.Clear();
    for (ULONG i = 0; i < SW_RX_BURST_MAX && (self->m_uart.StatusIsr(0) & SW_UART_ST_RXRDY); ++i) {
        self->m_uart.ReadDataIsr(0);
    }
    if (self->m_uart.StatusIsr(0) & SW_UART_ST_ERRORS) {
        self->m_uart.ErrorResetIsr(0);
    }
    self->m_midiInPort = c->Port;
    self->m_midiInGroup = c->Group;
    self->m_notReadyRun[SW_UART_CHANNELS] = 0;
    if ((self->m_faultMask & SW_TRPIF_UART0_RX) == 0) {
        self->m_hw.ModifyTrpifEnableSynced(0, SW_TRPIF_UART0_RX);
    }
    return STATUS_SUCCESS;
}

NTSTATUS
CSw1000Adapter::MidiInStopSynced(_In_ PINTERRUPTSYNC InterruptSync, _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(InterruptSync);
    CSw1000Adapter *self = static_cast<SW_MIDIIN_CONTEXT *>(Context)->Adapter;
    self->m_hw.ModifyTrpifEnableSynced(SW_TRPIF_UART0_RX, 0);
    self->m_midiInPort = nullptr;
    self->m_midiInGroup = nullptr;
    return STATUS_SUCCESS;
}

NTSTATUS
CSw1000Adapter::MidiReadSynced(_In_ PINTERRUPTSYNC InterruptSync, _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(InterruptSync);
    SW_MIDIIN_CONTEXT *c = static_cast<SW_MIDIIN_CONTEXT *>(Context);
    c->Done = c->Adapter->m_midiIn.Pop(c->Buffer, c->Length);
    return STATUS_SUCCESS;
}

/*
 * The data is copied to a stack buffer in small chunks first: the caller's
 * buffer may be pageable, and the copy into the queue runs at DIRQL. A chunk
 * goes in whole or not at all, so a full queue splits a Write only at chunk
 * boundaries.
 */
#define SW_MIDI_WRITE_CHUNK 64u

STDMETHODIMP_(NTSTATUS)
CSw1000Adapter::MidiWrite(_In_ ULONG Port, _In_reads_bytes_(Length) PVOID Data,
                          _In_ ULONG Length, _Out_ PULONG Written)
{
    *Written = 0;
    if (Port > SW_MIDI_PORT_OUT || Data == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
    if (m_interruptSync == nullptr) {
        return STATUS_DEVICE_NOT_READY;
    }
    BOOLEAN socket = (Port == SW_MIDI_PORT_OUT);

    const UCHAR *src = static_cast<const UCHAR *>(Data);
    while (*Written < Length) {
        UCHAR chunk[SW_MIDI_WRITE_CHUNK];
        ULONG n = min(Length - *Written, SW_MIDI_WRITE_CHUNK);
        RtlCopyMemory(chunk, src + *Written, n);

        SW_ENQUEUE_CONTEXT c = { this,
                                 socket ? &m_extTx : &m_synthTx,
                                 socket ? SW_TRPIF_UART0_TX : SW_TRPIF_UART1_TX,
                                 socket ? 0 : Port,
                                 chunk, n, (BOOLEAN)(*Written == 0), 0 };
        m_interruptSync->CallSynchronizedRoutine(EnqueueSynced, &c);
        if (c.Queued == 0) {
            break;                          /* full: the port driver retries the rest */
        }
        *Written += c.Queued;
    }
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CSw1000Adapter::MidiInStart(_In_ PPORTMIDI Port, _In_ PSERVICEGROUP ServiceGroup)
{
    if (m_interruptSync == nullptr) {
        return STATUS_DEVICE_NOT_READY;
    }
    SW_MIDIIN_CONTEXT c = { this, Port, ServiceGroup, nullptr, 0, 0 };
    m_interruptSync->CallSynchronizedRoutine(MidiInStartSynced, &c);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(void)
CSw1000Adapter::MidiInStop()
{
    if (m_interruptSync != nullptr) {
        SW_MIDIIN_CONTEXT c = { this, nullptr, nullptr, nullptr, 0, 0 };
        m_interruptSync->CallSynchronizedRoutine(MidiInStopSynced, &c);
    }
}

/* Received bytes leave the ring at DIRQL into a stack chunk, then go to the
 * caller's buffer at the caller's IRQL. */
STDMETHODIMP_(NTSTATUS)
CSw1000Adapter::MidiRead(_Out_writes_bytes_to_(Length, *Read) PVOID Buffer, _In_ ULONG Length,
                         _Out_ PULONG Read)
{
    *Read = 0;
    if (m_interruptSync == nullptr) {
        return STATUS_DEVICE_NOT_READY;
    }
    PUCHAR dst = static_cast<PUCHAR>(Buffer);
    while (*Read < Length) {
        UCHAR chunk[SW_MIDI_WRITE_CHUNK];
        SW_MIDIIN_CONTEXT c = { this, nullptr, nullptr, chunk, min(Length - *Read, SW_MIDI_WRITE_CHUNK), 0 };
        m_interruptSync->CallSynchronizedRoutine(MidiReadSynced, &c);
        if (c.Done == 0) {
            break;
        }
        RtlCopyMemory(dst + *Read, chunk, c.Done);
        *Read += c.Done;
    }
    return STATUS_SUCCESS;
}

/* ---- IAdapterPowerManagement (M8 will implement these; re/13) ------------ */

STDMETHODIMP_(void)
CSw1000Adapter::PowerChangeState(_In_ POWER_STATE NewState)
{
    UNREFERENCED_PARAMETER(NewState);
    /* M8: D3 quiesce / D0 cold re-init. */
}

STDMETHODIMP_(NTSTATUS)
CSw1000Adapter::QueryPowerChangeState(_In_ POWER_STATE NewStateQuery)
{
    UNREFERENCED_PARAMETER(NewStateQuery);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CSw1000Adapter::QueryDeviceCapabilities(_In_ PDEVICE_CAPABILITIES Caps)
{
    UNREFERENCED_PARAMETER(Caps);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------- */

NTSTATUS
NewSw1000Adapter(_Out_ PUNKNOWN *OutUnknown, _In_opt_ PUNKNOWN OuterUnknown)
{
    PAGED_CODE();

    if (OutUnknown == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Non-paged (spin locks inside); ExAllocatePool2 zeroes the memory. */
    CSw1000Adapter *adapter =
        new (POOL_FLAG_NON_PAGED, SW1000_POOLTAG) CSw1000Adapter(OuterUnknown);
    if (adapter == nullptr) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *OutUnknown = static_cast<PUNKNOWN>(static_cast<ISw1000Adapter*>(adapter));
    (*OutUnknown)->AddRef();
    return STATUS_SUCCESS;
}
