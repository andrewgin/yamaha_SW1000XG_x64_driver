/*
 * newdelete.h - the driver's operator new/delete.
 *
 * Follows the WDK SimpleAudioSample pattern: allocations use POOL_FLAGS and
 * ExAllocatePool2 (zeroed by default) rather than stdunk.h's legacy POOL_TYPE
 * form, and the sized/array deletes the C++ compiler emits are provided. Every
 * object is allocated with SW1000_POOLTAG, so the untagged deletes free with it.
 * Guarded the same way as the sample (stdunk.h defines the macro).
 */

#ifndef SW1000_NEWDELETE_H
#define SW1000_NEWDELETE_H

#ifdef _NEW_DELETE_OPERATORS_

PVOID operator new(size_t iSize, POOL_FLAGS poolFlags, ULONG tag);
PVOID operator new(size_t iSize, POOL_FLAGS poolFlags);

void __cdecl operator delete(PVOID pVoid, ULONG tag);
void __cdecl operator delete(_Pre_maybenull_ __drv_freesMem(Mem) PVOID pVoid, _In_ size_t cbSize);
void __cdecl operator delete[](_Pre_maybenull_ __drv_freesMem(Mem) PVOID pVoid, _In_ size_t cbSize);
void __cdecl operator delete[](_Pre_maybenull_ __drv_freesMem(Mem) PVOID pVoid);

#endif /* _NEW_DELETE_OPERATORS_ */

#endif /* SW1000_NEWDELETE_H */
