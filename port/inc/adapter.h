/*
 * adapter.h - the SW1000XG adapter object's private interface.
 *
 * PortCls only requires the adapter to be a COM object exposing IUnknown and
 * (for power) IAdapterPowerManagement. ISw1000Adapter is OUR OWN private
 * interface - it is how driver.cpp drives the hardware layer. It is not the
 * original's 120-slot IAdapterCommon vtable (clean-room, our own design, re/03).
 */

#ifndef SW1000_ADAPTER_H
#define SW1000_ADAPTER_H

#include "common.h"
#include "diag.h"

/* Our own interface identity (freshly minted; not from the original binary). */
/* {7F3C1A90-2B4D-4E6F-8A1C-9D0E5F6A7B8C} */
DEFINE_GUID(IID_ISw1000Adapter,
    0x7f3c1a90, 0x2b4d, 0x4e6f, 0x8a, 0x1c, 0x9d, 0x0e, 0x5f, 0x6a, 0x7b, 0x8c);

DECLARE_INTERFACE_(ISw1000Adapter, IUnknown)
{
    DEFINE_ABSTRACT_UNKNOWN()

    /* Bring the card up stage by stage (diag.h), logging each to Diag.
     * Extended per milestone. */
    STDMETHOD_(NTSTATUS, Init)(THIS_
        _In_ PRESOURCELIST ResourceList,
        _In_ PDEVICE_OBJECT DeviceObject,
        _In_ CSwDiag *Diag) PURE;

    /* Queue MIDI bytes for Port: 0-2 = the XG synth's port groups SWXG1-3 on
     * UART 1, SW_MIDI_PORT_OUT = the MIDI OUT socket on UART 0. Never blocks:
     * *Written may be less than Length when the transmit buffer is full (the
     * MIDI port driver retries). IRQL <= DISPATCH_LEVEL. */
    STDMETHOD_(NTSTATUS, MidiWrite)(THIS_
        _In_ ULONG Port,
        _In_reads_bytes_(Length) PVOID Data,
        _In_ ULONG Length,
        _Out_ PULONG Written) PURE;

    /* MIDI IN (UART 0 receive), driven by the capture stream's state: Start on
     * RUN turns the receive interrupt on, and each received burst calls
     * Port->Notify(ServiceGroup); Stop turns it off and forgets the port. The
     * stream's Read takes the received bytes. IRQL <= DISPATCH_LEVEL. */
    STDMETHOD_(NTSTATUS, MidiInStart)(THIS_
        _In_ PPORTMIDI Port,
        _In_ PSERVICEGROUP ServiceGroup) PURE;
    STDMETHOD_(void, MidiInStop)(THIS) PURE;
    STDMETHOD_(NTSTATUS, MidiRead)(THIS_
        _Out_writes_bytes_to_(Length, *Read) PVOID Buffer,
        _In_ ULONG Length,
        _Out_ PULONG Read) PURE;
};

#define SW_SWXG_PORTS     3u    /* SWXG1-3, selected on UART 1 by F5 01..03 (re/10) */
#define SW_MIDI_PORT_OUT  3u    /* MidiWrite port number of the MIDI OUT socket */

typedef ISw1000Adapter *PSW1000ADAPTER;

/* Factory: creates the adapter (non-paged; it holds spin locks) and returns its
 * IUnknown with one reference. */
NTSTATUS NewSw1000Adapter(
    _Out_ PUNKNOWN  *OutUnknown,
    _In_opt_ PUNKNOWN OuterUnknown);

#endif /* SW1000_ADAPTER_H */
