/*
 * driver.cpp - DriverEntry, AddDevice, StartDevice (M0).
 *
 *
 * Deliberate fixes vs. the original:
 *   - default-deny dispatch. The original left the parsed device ID
 *     uninitialised on a hardware-ID prefix mismatch, so stack garbage
 *     chose the branch. We dispatch only the SW1000XG ID and fail the rest.
 *   - every allocation is checked.
 *
 * Clean-room: no Yamaha code; this is standard PortCls scaffolding plus our own
 * device-ID parse.
 */

#include "adapter.h"
#include "midi.h"

extern "C" DRIVER_INITIALIZE DriverEntry;

static NTSTATUS AddDevice(_In_ PDRIVER_OBJECT DriverObject,
                          _In_ PDEVICE_OBJECT PhysicalDeviceObject);

static NTSTATUS StartDevice(_In_ PDEVICE_OBJECT DeviceObject,
                            _In_ PIRP Irp,
                            _In_ PRESOURCELIST ResourceList);

static NTSTATUS StartAdapter(_In_ PDEVICE_OBJECT DeviceObject,
                             _In_ PIRP Irp,
                             _In_ PRESOURCELIST ResourceList,
                             _In_ CSwDiag *Diag);

static NTSTATUS GetPciDeviceId(_In_ PDEVICE_OBJECT DeviceObject,
                               _Out_ PUSHORT Vendor,
                               _Out_ PUSHORT Device);

/*
 * Section placement uses #pragma code_seg (as the WDK sysvad sample does):
 * #pragma alloc_text only accepts C-linkage functions, and these are C++.
 */

/* ------------------------------------------------------------------------- */

/* The service key path, copied into a fixed buffer so it outlives DriverEntry
 * (RegistryPath is not guaranteed to persist). blobs.cpp reads the microcode
 * bundle from <this>\Parameters. */
static WCHAR    g_RegistryPathBuffer[512];
UNICODE_STRING  g_RegistryPath = { 0, sizeof(g_RegistryPathBuffer), g_RegistryPathBuffer };

#pragma code_seg("INIT")
extern "C" NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    if (RegistryPath != nullptr &&
        RegistryPath->Length <= sizeof(g_RegistryPathBuffer) - sizeof(WCHAR)) {
        RtlCopyUnicodeString(&g_RegistryPath, RegistryPath);
    } else {
        g_RegistryPath.Length = 0;   /* microcode load will fail cleanly (H-009) */
    }

    /* PortCls installs its own dispatch/unload handlers and calls AddDevice. */
    return PcInitializeAdapterDriver(DriverObject, RegistryPath,
                                     (PDRIVER_ADD_DEVICE)AddDevice);
}

#pragma code_seg("PAGE")
static NTSTATUS
AddDevice(_In_ PDRIVER_OBJECT DriverObject, _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    PAGED_CODE();

    /*
     * DeviceExtensionSize = 0: PortCls default. Phase 1 needs no custom device
     * extension - the adapter is created and consumed within StartDevice and
     * kept alive by its port-driver / power references. The original stashed the
     * adapter pointer at DeviceExtension+0x100 only for the ASIO cross-device
     * lookup (Phase 2, re/12); that will add a verified extension when built.
     */
    return PcAddAdapterDevice(DriverObject, PhysicalDeviceObject,
                              (PCPFNSTARTDEVICE)StartDevice,
                              SW1000_MAX_MINIPORTS, 0);
}

static NTSTATUS
StartDevice(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_ PRESOURCELIST ResourceList)
{
    PAGED_CODE();

    /* Every start is logged to the registry for the hardware test rounds
     * (diag.h). Paged: StartDevice runs at PASSIVE_LEVEL. */
    CSwDiag *diag = new (POOL_FLAG_PAGED, SW1000_POOLTAG) CSwDiag();
    if (diag == nullptr) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    diag->StartSession();
    NTSTATUS status = StartAdapter(DeviceObject, Irp, ResourceList, diag);
    diag->EndSession(status);
    delete diag;
    return status;
}

static NTSTATUS
StartAdapter(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_ PRESOURCELIST ResourceList,
             _In_ CSwDiag *Diag)
{
    PAGED_CODE();

    USHORT vendor = 0, device = 0;
    NTSTATUS status = GetPciDeviceId(DeviceObject, &vendor, &device);
    if (!NT_SUCCESS(status)) {
        Diag->Log("hardware ID unreadable (0x%08x)", status);
        return status;
    }
    Diag->Log("device PCI %04x:%04x", (ULONG)vendor, (ULONG)device);

    /* H-012: default-deny. v1 supports the SW1000XG only. */
    if (vendor != SW1000_VENDOR_ID || device != SW1000_DEVICE_ID) {
        Diag->Log("unsupported device");
        return STATUS_NOT_SUPPORTED;
    }

    PUNKNOWN unknownAdapter = nullptr;
    status = NewSw1000Adapter(&unknownAdapter, nullptr);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    PSW1000ADAPTER adapter = nullptr;
    status = unknownAdapter->QueryInterface(IID_ISw1000Adapter, (PVOID*)&adapter);
    if (NT_SUCCESS(status)) {
        status = adapter->Init(ResourceList, DeviceObject, Diag);
        if (NT_SUCCESS(status)) {
            /* Registers the adapter for power IRPs and takes its own reference. */
            status = PcRegisterAdapterPowerManagement(unknownAdapter, DeviceObject);
        }
        adapter->Release();
    }

    /*
     * Stage 8 (M7): the MIDI subdevices (SWXG1-3, MIDIOUT, MIDIIN); each
     * miniport holds its own adapter reference. Wave and topology come later.
     */
    if (NT_SUCCESS(status) && Diag->BeginStage(SW_STAGE_MIDIPORTS)) {
        status = InstallMidiSubdevices(DeviceObject, Irp, unknownAdapter, ResourceList, Diag);
        Diag->EndStage(status);
    }

    /*
     * Drop our creation reference; on success PortCls holds the adapter through
     * power management and the miniports, on failure this frees it.
     */
    unknownAdapter->Release();
    return status;
}

/* ------------------------------------------------------------------------- */

NTSTATUS
SwOpenParametersKey(_In_ ACCESS_MASK Access, _In_ BOOLEAN Create, _Out_ PHANDLE Key)
{
    PAGED_CODE();

    *Key = nullptr;
    if (g_RegistryPath.Length == 0) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    /* Build "<service key>\Parameters". */
    static const WCHAR kSuffix[] = L"\\Parameters";
    USHORT bytes = (USHORT)(g_RegistryPath.Length + sizeof(kSuffix));
    PWCHAR buf = (PWCHAR)ExAllocatePool2(POOL_FLAG_PAGED, bytes, SW1000_POOLTAG);
    if (buf == nullptr) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    UNICODE_STRING path = { 0, bytes, buf };
    RtlCopyUnicodeString(&path, &g_RegistryPath);
    RtlAppendUnicodeToString(&path, kSuffix);

    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               nullptr, nullptr);
    NTSTATUS status = Create
        ? ZwCreateKey(Key, Access, &oa, 0, nullptr, REG_OPTION_NON_VOLATILE, nullptr)
        : ZwOpenKey(Key, Access, &oa);

    ExFreePoolWithTag(buf, SW1000_POOLTAG);
    return status;
}

/* Read up to four hex digits following `key` in a WCHAR buffer. */
static BOOLEAN
ParseHexField(_In_reads_(count) const WCHAR *buf, _In_ SIZE_T count,
              _In_ const WCHAR *key, _Out_ PUSHORT out)
{
    SIZE_T klen = 0;
    while (key[klen] != L'\0') {
        klen++;
    }

    for (SIZE_T i = 0; i + klen <= count; ++i) {
        SIZE_T j = 0;
        for (; j < klen; ++j) {
            if (buf[i + j] != key[j]) {
                break;
            }
        }
        if (j != klen) {
            continue;
        }

        ULONG val = 0;
        int digits = 0;
        for (SIZE_T k = i + klen; k < count && digits < 4; ++k) {
            WCHAR c = buf[k];
            ULONG nib;
            if (c >= L'0' && c <= L'9') {
                nib = (ULONG)(c - L'0');
            } else if (c >= L'A' && c <= L'F') {
                nib = (ULONG)(c - L'A') + 10;
            } else if (c >= L'a' && c <= L'f') {
                nib = (ULONG)(c - L'a') + 10;
            } else {
                break;
            }
            val = (val << 4) | nib;
            digits++;
        }
        if (digits > 0) {
            *out = (USHORT)val;
            return TRUE;
        }
    }
    return FALSE;
}

/*
 * Resolve VEN_/DEV_ from the PnP hardware ID (a REG_MULTI_SZ like
 * "PCI\VEN_1073&DEV_1000&SUBSYS_...&REV_..."). This mirrors the original, which
 * parsed the hardware-ID string; an alternative is a PCI config-space read via
 * BUS_INTERFACE_STANDARD. Validate on the WDK host.
 */
static NTSTATUS
GetPciDeviceId(_In_ PDEVICE_OBJECT DeviceObject, _Out_ PUSHORT Vendor, _Out_ PUSHORT Device)
{
    PAGED_CODE();

    *Vendor = 0;
    *Device = 0;

    ULONG length = 0;
    NTSTATUS status = PcGetDeviceProperty(DeviceObject, DevicePropertyHardwareID,
                                          0, nullptr, &length);
    if (status != STATUS_BUFFER_TOO_SMALL && status != STATUS_BUFFER_OVERFLOW) {
        return NT_SUCCESS(status) ? STATUS_UNSUCCESSFUL : status;
    }
    if (length < sizeof(WCHAR)) {
        return STATUS_UNSUCCESSFUL;
    }

    PWCHAR buf = (PWCHAR)ExAllocatePool2(POOL_FLAG_PAGED, length, SW1000_POOLTAG);
    if (buf == nullptr) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = PcGetDeviceProperty(DeviceObject, DevicePropertyHardwareID,
                                 length, buf, &length);
    if (NT_SUCCESS(status)) {
        SIZE_T count = length / sizeof(WCHAR);
        USHORT v = 0, d = 0;
        if (ParseHexField(buf, count, L"VEN_", &v) &&
            ParseHexField(buf, count, L"DEV_", &d)) {
            *Vendor = v;
            *Device = d;
        } else {
            status = STATUS_UNSUCCESSFUL;
        }
    }

    ExFreePoolWithTag(buf, SW1000_POOLTAG);
    return status;
}
#pragma code_seg()
