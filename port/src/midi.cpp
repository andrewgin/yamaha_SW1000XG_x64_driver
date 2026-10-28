/*
 * midi.cpp - the MIDI miniports (M7, see midi.h).
 *
 * Filter layouts (re/10, decoded from the original's descriptors):
 *   SWXG1-3  (0x2efa8) categories AUDIO + RENDER; pin 0 = MIDI render sink,
 *            at most 2 streams, music range (wavetable, 1110 channels, 64
 *            notes, mask 0xffff), name "MIDI"; pin 1 = MIDI-bus bridge out.
 *   MIDIOUT  (0x2efd8) as SWXG but 1 stream and a "port" range (0 channels,
 *            0 notes, mask 0xffff).
 *   MIDIIN   (0x2f008) categories AUDIO + CAPTURE; pin 0 = MIDI capture
 *            (flow OUT, sink), 1 stream, port range; pin 1 = bridge in;
 *            connection pin 1 -> pin 0.
 *
 * Render streams forward Write to the adapter: SWXG to the synth queue for
 * their port group (F5 01..03), MIDIOUT to the socket's queue. The MIDIIN
 * stream turns UART 0 receive on while it runs (RecordStart in the original)
 * and its Read takes the received bytes.
 *
 * Not yet: the original's note-off sweep when a render stream closes (re/10
 * records which channels saw note-ons). Clean-room; written from the notes.
 */

#include "midi.h"

/* ---- filter descriptor ---------------------------------------------------- */

static KSDATARANGE_MUSIC g_SwxgStreamRange = {
    {
        sizeof(KSDATARANGE_MUSIC), 0, 0, 0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_MUSIC),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_MIDI),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
    },
    STATICGUIDOF(KSMUSIC_TECHNOLOGY_WAVETABLE),
    1110,                                   /* Channels: as the original */
    64,                                     /* Notes: 64-voice polyphony */
    0xFFFF                                  /* ChannelMask */
};

static KSDATARANGE g_SwxgBridgeRange = {
    sizeof(KSDATARANGE), 0, 0, 0,
    STATICGUIDOF(KSDATAFORMAT_TYPE_MUSIC),
    STATICGUIDOF(KSDATAFORMAT_SUBTYPE_MIDI_BUS),
    STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
};

static KSDATARANGE_MUSIC g_SocketStreamRange = {
    {
        sizeof(KSDATARANGE_MUSIC), 0, 0, 0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_MUSIC),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_MIDI),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
    },
    STATICGUIDOF(KSMUSIC_TECHNOLOGY_PORT),
    0, 0, 0xFFFF                            /* as the original */
};

static PKSDATARANGE g_SwxgStreamRanges[] = { PKSDATARANGE(&g_SwxgStreamRange) };
static PKSDATARANGE g_SocketStreamRanges[] = { PKSDATARANGE(&g_SocketStreamRange) };
static PKSDATARANGE g_SwxgBridgeRanges[] = { &g_SwxgBridgeRange };

static PCPIN_DESCRIPTOR g_SwxgPins[] = {
    {
        2, 2, 0, nullptr,                   /* max global, max filter, min, automation */
        {
            0, nullptr, 0, nullptr,         /* interfaces, mediums */
            ARRAYSIZE(g_SwxgStreamRanges), g_SwxgStreamRanges,
            KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO, &KSAUDFNAME_MIDI, 0
        }
    },
    {
        0, 0, 0, nullptr,
        {
            0, nullptr, 0, nullptr,
            ARRAYSIZE(g_SwxgBridgeRanges), g_SwxgBridgeRanges,
            KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO, nullptr, 0
        }
    },
};

static PCCONNECTION_DESCRIPTOR g_SwxgConnections[] = {
    { PCFILTER_NODE, 0, PCFILTER_NODE, 1 },
};

static GUID g_SwxgCategories[] = {
    STATICGUIDOF(KSCATEGORY_AUDIO),
    STATICGUIDOF(KSCATEGORY_RENDER),
};

static PCFILTER_DESCRIPTOR g_SwxgFilter = {
    0, nullptr,                                         /* version, automation */
    sizeof(PCPIN_DESCRIPTOR), ARRAYSIZE(g_SwxgPins), g_SwxgPins,
    sizeof(PCNODE_DESCRIPTOR), 0, nullptr,              /* no nodes */
    ARRAYSIZE(g_SwxgConnections), g_SwxgConnections,
    ARRAYSIZE(g_SwxgCategories), g_SwxgCategories,
};

/* MIDIOUT: the SWXG layout with one stream and the port range. */
static PCPIN_DESCRIPTOR g_MidiOutPins[] = {
    {
        1, 1, 0, nullptr,
        {
            0, nullptr, 0, nullptr,
            ARRAYSIZE(g_SocketStreamRanges), g_SocketStreamRanges,
            KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO, &KSAUDFNAME_MIDI, 0
        }
    },
    {
        0, 0, 0, nullptr,
        {
            0, nullptr, 0, nullptr,
            ARRAYSIZE(g_SwxgBridgeRanges), g_SwxgBridgeRanges,
            KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO, nullptr, 0
        }
    },
};

static PCFILTER_DESCRIPTOR g_MidiOutFilter = {
    0, nullptr,
    sizeof(PCPIN_DESCRIPTOR), ARRAYSIZE(g_MidiOutPins), g_MidiOutPins,
    sizeof(PCNODE_DESCRIPTOR), 0, nullptr,
    ARRAYSIZE(g_SwxgConnections), g_SwxgConnections,
    ARRAYSIZE(g_SwxgCategories), g_SwxgCategories,
};

/* MIDIIN: capture pin out of the filter, fed by the bridge pin. */
static PCPIN_DESCRIPTOR g_MidiInPins[] = {
    {
        1, 1, 0, nullptr,
        {
            0, nullptr, 0, nullptr,
            ARRAYSIZE(g_SocketStreamRanges), g_SocketStreamRanges,
            KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO, &KSAUDFNAME_MIDI, 0
        }
    },
    {
        0, 0, 0, nullptr,
        {
            0, nullptr, 0, nullptr,
            ARRAYSIZE(g_SwxgBridgeRanges), g_SwxgBridgeRanges,
            KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO, nullptr, 0
        }
    },
};

static PCCONNECTION_DESCRIPTOR g_MidiInConnections[] = {
    { PCFILTER_NODE, 1, PCFILTER_NODE, 0 },
};

static GUID g_MidiInCategories[] = {
    STATICGUIDOF(KSCATEGORY_AUDIO),
    STATICGUIDOF(KSCATEGORY_CAPTURE),
};

static PCFILTER_DESCRIPTOR g_MidiInFilter = {
    0, nullptr,
    sizeof(PCPIN_DESCRIPTOR), ARRAYSIZE(g_MidiInPins), g_MidiInPins,
    sizeof(PCNODE_DESCRIPTOR), 0, nullptr,
    ARRAYSIZE(g_MidiInConnections), g_MidiInConnections,
    ARRAYSIZE(g_MidiInCategories), g_MidiInCategories,
};

typedef enum _SW_MIDI_KIND {
    SwMidiSynth,                            /* SWXG1-3: UART 1, port group */
    SwMidiOut,                              /* MIDIOUT socket: UART 0 TX */
    SwMidiIn,                               /* MIDIIN socket: UART 0 RX */
} SW_MIDI_KIND;

/* ---- classes --------------------------------------------------------------- */

class CMiniportMidiSw : public IMiniportMidi, public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportMidiSw);
    ~CMiniportMidiSw();

    IMP_IMiniportMidi;

    SW_MIDI_KIND   m_kind = SwMidiSynth;
    ULONG          m_portGroup = 0;             /* SWXG: 0-2 */
    PSW1000ADAPTER m_adapter = nullptr;
    PPORTMIDI      m_portMidi = nullptr;        /* not referenced: it owns us */
    PSERVICEGROUP  m_serviceGroup = nullptr;
};

class CMiniportMidiStreamSw : public IMiniportMidiStream, public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportMidiStreamSw);
    ~CMiniportMidiStreamSw();

    IMP_IMiniportMidiStream;

    CMiniportMidiSw *m_miniport = nullptr;    /* referenced */
    BOOLEAN          m_capture = FALSE;
    BOOLEAN          m_receiving = FALSE;       /* MIDI IN running */
};

/* ---- miniport ------------------------------------------------------------- */

CMiniportMidiSw::~CMiniportMidiSw()
{
    if (m_serviceGroup != nullptr) {
        m_serviceGroup->Release();
    }
    if (m_adapter != nullptr) {
        m_adapter->Release();
    }
}

STDMETHODIMP_(NTSTATUS)
CMiniportMidiSw::NonDelegatingQueryInterface(_In_ REFIID Interface, _COM_Outptr_ PVOID *Object)
{
    if (IsEqualGUIDAligned(Interface, IID_IUnknown)) {
        *Object = PVOID(PUNKNOWN(PMINIPORTMIDI(this)));
    } else if (IsEqualGUIDAligned(Interface, IID_IMiniport)) {
        *Object = PVOID(PMINIPORT(this));
    } else if (IsEqualGUIDAligned(Interface, IID_IMiniportMidi)) {
        *Object = PVOID(PMINIPORTMIDI(this));
    } else {
        *Object = nullptr;
        return STATUS_INVALID_PARAMETER;
    }
    PUNKNOWN(*Object)->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportMidiSw::GetDescription(_Out_ PPCFILTER_DESCRIPTOR *Description)
{
    *Description = (m_kind == SwMidiIn)  ? &g_MidiInFilter :
                   (m_kind == SwMidiOut) ? &g_MidiOutFilter : &g_SwxgFilter;
    return STATUS_SUCCESS;
}

/* The port driver's default intersection handles a plain MIDI range. */
STDMETHODIMP_(NTSTATUS)
CMiniportMidiSw::DataRangeIntersection(_In_ ULONG PinId, _In_ PKSDATARANGE DataRange,
                                         _In_ PKSDATARANGE MatchingDataRange,
                                         _In_ ULONG OutputBufferLength,
                                         _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength) PVOID ResultantFormat,
                                         _Out_ PULONG ResultantFormatLength)
{
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(DataRange);
    UNREFERENCED_PARAMETER(MatchingDataRange);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ResultantFormat);
    UNREFERENCED_PARAMETER(ResultantFormatLength);
    return STATUS_NOT_IMPLEMENTED;
}

STDMETHODIMP_(NTSTATUS)
CMiniportMidiSw::Init(_In_ PUNKNOWN UnknownAdapter, _In_ PRESOURCELIST ResourceList,
                        _In_ PPORTMIDI Port, _Out_ PSERVICEGROUP *ServiceGroup)
{
    UNREFERENCED_PARAMETER(ResourceList);
    *ServiceGroup = nullptr;
    m_portMidi = Port;

    NTSTATUS status = UnknownAdapter->QueryInterface(IID_ISw1000Adapter, (PVOID *)&m_adapter);
    if (NT_SUCCESS(status)) {
        status = PcNewServiceGroup(&m_serviceGroup, nullptr);
    }
    if (NT_SUCCESS(status)) {
        *ServiceGroup = m_serviceGroup;
        m_serviceGroup->AddRef();
    }
    return status;
}

STDMETHODIMP_(void)
CMiniportMidiSw::Service(void)
{
    /* Nothing to do here: the port driver calls the capture stream's Read
     * after the ISR's Notify, and the ISR drains the transmit queues. */
}

STDMETHODIMP_(NTSTATUS)
CMiniportMidiSw::NewStream(_Out_ PMINIPORTMIDISTREAM *Stream, _In_opt_ PUNKNOWN OuterUnknown,
                             _In_ POOL_TYPE PoolType, _In_ ULONG Pin, _In_ BOOLEAN Capture,
                             _In_ PKSDATAFORMAT DataFormat, _Out_ PSERVICEGROUP *ServiceGroup)
{
    UNREFERENCED_PARAMETER(PoolType);
    UNREFERENCED_PARAMETER(DataFormat);
    *Stream = nullptr;
    *ServiceGroup = nullptr;
    if (Pin != 0 || Capture != (m_kind == SwMidiIn)) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    CMiniportMidiStreamSw *stream =
        new (POOL_FLAG_NON_PAGED, SW1000_POOLTAG) CMiniportMidiStreamSw(OuterUnknown);
    if (stream == nullptr) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    stream->AddRef();
    stream->m_miniport = this;
    stream->m_capture = Capture;
    AddRef();
    *Stream = PMINIPORTMIDISTREAM(stream);
    (*Stream)->AddRef();
    if (Capture && m_serviceGroup != nullptr) {
        *ServiceGroup = m_serviceGroup;     /* Notify reaches this stream's Read */
        m_serviceGroup->AddRef();
    }
    stream->Release();
    return STATUS_SUCCESS;
}

/* ---- stream --------------------------------------------------------------- */

CMiniportMidiStreamSw::~CMiniportMidiStreamSw()
{
    if (m_receiving) {
        m_miniport->m_adapter->MidiInStop();
    }
    if (m_miniport != nullptr) {
        m_miniport->Release();
    }
}

STDMETHODIMP_(NTSTATUS)
CMiniportMidiStreamSw::NonDelegatingQueryInterface(_In_ REFIID Interface, _COM_Outptr_ PVOID *Object)
{
    if (IsEqualGUIDAligned(Interface, IID_IUnknown)) {
        *Object = PVOID(PUNKNOWN(PMINIPORTMIDISTREAM(this)));
    } else if (IsEqualGUIDAligned(Interface, IID_IMiniportMidiStream)) {
        *Object = PVOID(PMINIPORTMIDISTREAM(this));
    } else {
        *Object = nullptr;
        return STATUS_INVALID_PARAMETER;
    }
    PUNKNOWN(*Object)->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportMidiStreamSw::SetFormat(_In_ PKSDATAFORMAT DataFormat)
{
    UNREFERENCED_PARAMETER(DataFormat);
    return STATUS_SUCCESS;
}

/* Render: no action, output flows whenever Write is called (re/10). Capture:
 * RUN turns MIDI IN on, any other state turns it off (the original's
 * RecordStart / Stop; ACQUIRE's queue clear happens on the next start). */
STDMETHODIMP_(NTSTATUS)
CMiniportMidiStreamSw::SetState(_In_ KSSTATE State)
{
    if (!m_capture) {
        return STATUS_SUCCESS;
    }
    if (State == KSSTATE_RUN && !m_receiving) {
        NTSTATUS status = m_miniport->m_adapter->MidiInStart(m_miniport->m_portMidi,
                                                             m_miniport->m_serviceGroup);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        m_receiving = TRUE;
    } else if (State != KSSTATE_RUN && m_receiving) {
        m_miniport->m_adapter->MidiInStop();
        m_receiving = FALSE;
    }
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportMidiStreamSw::Read(_In_ PVOID BufferAddress, _In_ ULONG BufferLength, _Out_ PULONG BytesRead)
{
    *BytesRead = 0;
    if (!m_capture) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    return m_miniport->m_adapter->MidiRead(BufferAddress, BufferLength, BytesRead);
}

/* May be called at DISPATCH_LEVEL; MidiWrite never blocks. */
STDMETHODIMP_(NTSTATUS)
CMiniportMidiStreamSw::Write(_In_ PVOID BufferAddress, _In_ ULONG BytesToWrite, _Out_ PULONG BytesWritten)
{
    *BytesWritten = 0;
    if (m_capture) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    ULONG port = (m_miniport->m_kind == SwMidiOut) ? SW_MIDI_PORT_OUT : m_miniport->m_portGroup;
    return m_miniport->m_adapter->MidiWrite(port, BufferAddress, BytesToWrite, BytesWritten);
}

/* ---- installation ------------------------------------------------------- */

#pragma code_seg("PAGE")
NTSTATUS
InstallMidiSubdevices(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp,
                      _In_ PUNKNOWN UnknownAdapter, _In_ PRESOURCELIST ResourceList,
                      _In_ CSwDiag *Diag)
{
    PAGED_CODE();

    /* Reference strings as in the INF (and the original, re/02). */
    static const struct {
        PCWSTR       name;
        const char  *label;
        SW_MIDI_KIND kind;
        ULONG        portGroup;
    } kSubdevices[] = {
        { L"SWXG1",   "SWXG1 (F5 01)",       SwMidiSynth, 0 },
        { L"SWXG2",   "SWXG2 (F5 02)",       SwMidiSynth, 1 },
        { L"SWXG3",   "SWXG3 (F5 03)",       SwMidiSynth, 2 },
        { L"MIDIOUT", "MIDIOUT (UART 0 TX)", SwMidiOut,   0 },
        { L"MIDIIN",  "MIDIIN (UART 0 RX)",  SwMidiIn,    0 },
    };
    for (ULONG i = 0; i < ARRAYSIZE(kSubdevices); ++i) {
        PPORT    port = nullptr;
        PUNKNOWN miniport = nullptr;

        NTSTATUS status = PcNewPort(&port, CLSID_PortMidi);
        if (NT_SUCCESS(status)) {
            CMiniportMidiSw *m = new (POOL_FLAG_NON_PAGED, SW1000_POOLTAG) CMiniportMidiSw(nullptr);
            if (m == nullptr) {
                status = STATUS_INSUFFICIENT_RESOURCES;
            } else {
                m->m_kind = kSubdevices[i].kind;
                m->m_portGroup = kSubdevices[i].portGroup;
                miniport = PUNKNOWN(PMINIPORTMIDI(m));
                miniport->AddRef();
            }
        }
        if (NT_SUCCESS(status)) {
            status = port->Init(DeviceObject, Irp, miniport, UnknownAdapter, ResourceList);
        }
        if (NT_SUCCESS(status)) {
            status = PcRegisterSubdevice(DeviceObject, const_cast<PWSTR>(kSubdevices[i].name), port);
        }
        Diag->Log("  %s: 0x%08x", kSubdevices[i].label, status);

        if (miniport != nullptr) {
            miniport->Release();
        }
        if (port != nullptr) {
            port->Release();
        }
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }
    return STATUS_SUCCESS;
}
#pragma code_seg()
