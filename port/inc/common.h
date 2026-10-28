/*
 * common.h - shared definitions for the SW1000XG x64 PortCls driver.
 *
 * CLEAN-ROOM: this driver is written from the behaviour notes in ../re, not
 * from any Yamaha code or decompiler output. No Yamaha bytes are included.
 *
 * Phase 1 target: Windows 10/11 x64, WDM/PortCls, SW1000XG only.
 */

#ifndef SW1000_COMMON_H
#define SW1000_COMMON_H

/*
 * Same include set as the WDK SimpleAudioSample: portcls.h brings in wdm.h and
 * the KS headers it needs; stdunk.h gives CUnknown. newdelete.h supplies the
 * driver's own operator new/delete (POOL_FLAGS + ExAllocatePool2).
 */
#include <portcls.h>
#include <stdunk.h>
#include "newdelete.h"

/* Pool tag: shows as "SWXG" in a pool dump (tags print little-endian). */
#define SW1000_POOLTAG       ((ULONG)'GXWS')

/* PCI identity. SW1000XG only in v1; DS2416 (DEV_2000) is out of scope (re/02). */
#define SW1000_VENDOR_ID     ((USHORT)0x1073)
#define SW1000_DEVICE_ID     ((USHORT)0x1000)

/* Upper bound on PortCls objects per adapter. Original passed MaxObjects=20. */
#define SW1000_MAX_MINIPORTS 20

/* The driver's service registry key, saved in DriverEntry; the microcode bundle
 * (blobs.cpp) and the diagnostics values (diag.cpp) live under <this>\Parameters. */
extern UNICODE_STRING g_RegistryPath;

/* Open <service key>\Parameters (PASSIVE_LEVEL). With Create, the subkey is
 * created if missing. The caller closes the handle with ZwClose. */
NTSTATUS SwOpenParametersKey(_In_ ACCESS_MASK Access, _In_ BOOLEAN Create, _Out_ PHANDLE Key);

/* Interrupt time in 100-ns units, at QPC resolution (KeQueryInterruptTime only
 * advances once per clock tick, 15.6 ms by default: round 1 showed it). */
__forceinline ULONG64 SwNow100ns()
{
    ULONG64 qpc;
    return KeQueryInterruptTimePrecise(&qpc);
}

#if DBG
#define SwTrace(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_INFO_LEVEL, "ysw1000xg: " fmt, __VA_ARGS__)
#else
#define SwTrace(fmt, ...) ((void)0)
#endif

#endif /* SW1000_COMMON_H */
