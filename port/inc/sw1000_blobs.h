/*
 * sw1000_blobs.h - DSP microcode manifest and loader.
 *
 * The ~9 KB of Yamaha DSP tables CANNOT be
 * redistributed. They are extracted at install time from the user's own
 * yswds.sys (hash-verified) and delivered to the driver as one REG_BINARY bundle
 * (manifest order, concatenated). NO Yamaha bytes live in this repo.
 *
 * THIS FILE contains only facts about those tables - names, byte lengths, the
 * SHA-256 of each, and where each goes in the DSP. That is our own
 * descriptive manifest, not Yamaha code.
 */

#ifndef SW1000_BLOBS_H
#define SW1000_BLOBS_H

#include "common.h"

/* Blob identity. The install-time bundle is these blobs concatenated in this
 * exact order (the extractor must match it). */
typedef enum _SW_BLOB {
    SW_BLOB_BCR1MEL = 0,      /* MPR slot 0 (bcr)   */
    SW_BLOB_DEQC1,            /* MPR slot 1 (deqc)  */
    SW_BLOB_DEQM1,            /* MPR slot 2 (deqm)  */
    SW_BLOB_DSPC1,            /* MPR slot 3 (dspc)  */
    SW_BLOB_DSPA1,            /* MPR slot 4 (dspa)  */
    SW_BLOB_DSPM1_0,          /* MPR slot 5 (dspm#0)*/
    SW_BLOB_DSPM1_1,          /* MPR slot 6 (dspm#1)*/
    SW_BLOB_N1MOD0,           /* MPR slot 7 (mod)   */
    SW_BLOB_N1CREG0CMEL,      /* MPR slot 8 (creg)  */
    SW_BLOB_N1FGTIM0,         /* MPR slot 9 (fgtim) */
    SW_BLOB_N1FGURAM0,        /* MPR slot 10 (fguram) */
    SW_BLOB_N1CREG0MEL,       /* SendCESCR, selector 0x800 */
    SW_BLOB_N1GREG0,          /* port-0 global regs (16 shorts) */
    SW_BLOB_N1GREG1,          /* port-1 */
    SW_BLOB_N1GREG2,          /* port-2 */
    SW_BLOB_N1GREG3,          /* port-3 */
    SW_BLOB_N1GREG4,          /* port-4 */
    /* (The n1mod0KeyOn/KeyOnOff words are the constants 0xffff and 0, written
     * as immediates by the original; see SW_MOD_KEY_ON/OFF. Not in the bundle.) */
    /* record/play format patches, used from M4/M5 (re/05, re/08) */
    SW_BLOB_PREREC_306F0,
    SW_BLOB_PREREC_226A0,
    SW_BLOB_PREREC_226CC,
    SW_BLOB_PREREC_226F8,
    SW_BLOB_PREREC_22724,
    SW_BLOB_PREREC_22734,
    SW_BLOB_PLAYMODE_DSPC_234D0,
    SW_BLOB_PLAYMODE_DSPC_234E0,
    SW_BLOB_RECMODE_DSPC_234F0,
    SW_BLOB_RECMODE_DSPC_234FC,
    SW_BLOB_PLAYMODE_ADDR_23508,
    SW_BLOB_PLAYMODE_ADDR_23510,
    SW_BLOB_PLAYMODE_ADDR_23518,
    SW_BLOB_RECMODE_ADDR_23520,
    SW_BLOB_RECMODE_ADDR_23524,
    SW_BLOB_RECMODE_ADDR_23528,
    SW_BLOB_RECMODE_ADDR_2352C,
    SW_BLOB_COUNT
} SW_BLOB;

typedef struct _SW_BLOB_DESC {
    const char *name;
    ULONG       length;        /* bytes */
    const char *sha256hex;     /* 64 lowercase hex chars (re/05) */
} SW_BLOB_DESC;

extern const SW_BLOB_DESC g_SwBlobs[SW_BLOB_COUNT];

/* MPR set-0 load order (re/05 selector<->MPR map). Selector = slot << 8. */
#define SW_MPR_SLOT_COUNT 11
extern const SW_BLOB g_SwMprSlotBlob[SW_MPR_SLOT_COUNT];
extern const ULONG   g_SwMprSlotWords[SW_MPR_SLOT_COUNT];

/*
 * Microcode bundle, loaded once per D0 entry. Reads the REG_BINARY the installer
 * wrote, checks total length against the manifest, slices it, and verifies every
 * blob's SHA-256. The driver refuses to start if it is absent or fails (H-009).
 */
class CSw1000Microcode
{
public:
    CSw1000Microcode() = default;
    ~CSw1000Microcode();

    NTSTATUS Load();                       /* read + slice + verify */
    void     Unload();
    BOOLEAN  IsLoaded() const { return m_data != nullptr; }

    /* Raw bytes of one blob (nullptr if not loaded). */
    const UCHAR *Blob(_In_ SW_BLOB id, _Out_opt_ PULONG lengthBytes) const;
    /* Same as 32-bit words (length must be a multiple of 4; all are). */
    const ULONG *BlobWords(_In_ SW_BLOB id, _Out_opt_ PULONG wordCount) const;

private:
    NTSTATUS ReadRegistryBundle(_Outptr_ PUCHAR *buffer, _Out_ PULONG length);
    NTSTATUS SliceAndCheck();
    NTSTATUS VerifyHashes();

    PUCHAR m_data = nullptr;
    ULONG  m_length = 0;
    ULONG  m_offset[SW_BLOB_COUNT] = {};
};

#endif /* SW1000_BLOBS_H */
