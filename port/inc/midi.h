/*
 * midi.h - the MIDI miniports (M7, re/10).
 *
 * Each MIDI subdevice is a PortCls MIDI filter (CLSID_PortMidi + our
 * IMiniportMidi). SWXG1-3 render to the XG synth's port groups on UART 1;
 * MIDIOUT renders to the MIDI OUT socket and MIDIIN captures from the MIDI IN
 * socket, both on UART 0. All go through ISw1000Adapter.
 *
 * Clean-room; the filter layout follows the original's descriptor as
 * decoded by tools/ks_descriptors.py (re/10), the code the WDK's PortCls
 * interfaces.
 */

#ifndef SW1000_MIDI_H
#define SW1000_MIDI_H

#include "adapter.h"

/* Create the MIDI port drivers and register them as subdevices "SWXG1",
 * "SWXG2", "SWXG3", "MIDIOUT", "MIDIIN" (the INF's interface reference
 * strings). PASSIVE_LEVEL. */
NTSTATUS InstallMidiSubdevices(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP           Irp,
    _In_ PUNKNOWN       UnknownAdapter,
    _In_ PRESOURCELIST  ResourceList,
    _In_ CSwDiag       *Diag);

#endif /* SW1000_MIDI_H */
