/*
 * newdelete.cpp - the driver's operator new/delete (see inc/newdelete.h).
 * Non-paged code: delete can run wherever an object's last Release happens.
 */

#include "common.h"

#ifdef _NEW_DELETE_OPERATORS_

#pragma code_seg()

PVOID operator new(size_t iSize, POOL_FLAGS poolFlags, ULONG tag)
{
    return ExAllocatePool2(poolFlags, iSize, tag);
}

PVOID operator new(size_t iSize, POOL_FLAGS poolFlags)
{
    return ExAllocatePool2(poolFlags, iSize, SW1000_POOLTAG);
}

void __cdecl operator delete(PVOID pVoid, ULONG tag)
{
    if (pVoid != nullptr) {
        ExFreePoolWithTag(pVoid, tag);
    }
}

void __cdecl operator delete(_Pre_maybenull_ __drv_freesMem(Mem) PVOID pVoid, _In_ size_t cbSize)
{
    UNREFERENCED_PARAMETER(cbSize);
    if (pVoid != nullptr) {
        ExFreePoolWithTag(pVoid, SW1000_POOLTAG);
    }
}

void __cdecl operator delete[](_Pre_maybenull_ __drv_freesMem(Mem) PVOID pVoid, _In_ size_t cbSize)
{
    UNREFERENCED_PARAMETER(cbSize);
    if (pVoid != nullptr) {
        ExFreePoolWithTag(pVoid, SW1000_POOLTAG);
    }
}

void __cdecl operator delete[](_Pre_maybenull_ __drv_freesMem(Mem) PVOID pVoid)
{
    if (pVoid != nullptr) {
        ExFreePoolWithTag(pVoid, SW1000_POOLTAG);
    }
}

#endif /* _NEW_DELETE_OPERATORS_ */
