/*
 * blobs.cpp - DSP microcode manifest and install-provided bundle loader (M2).
 *
 * Source: "Blob table" (names, byte lengths, SHA-256, DSP mapping).
 * The bundle is the REG_BINARY the installer wrote under the driver's service
 * Parameters key (value "Microcode"); the installer extracted it from the user's
 * own yswds.sys. H-009: no Yamaha bytes are shipped in this package; H-017: the
 * driver refuses to start if the bundle is absent or any hash fails.
 *
 * Clean-room: the manifest below is our own description (facts); the actual bytes
 * never enter the repo.
 */

#include "sw1000_blobs.h"
#include <bcrypt.h>            /* kernel CNG; link cng.lib (see BUILD.md) */

/* ---- manifest (re/05) --------------------------------------------------- */

const SW_BLOB_DESC g_SwBlobs[SW_BLOB_COUNT] = {
    { "bcr1MEL",    0x500, "eacd0e4ae466af86b2316468a65b97b996ceb82e72e860e8af32444f3e9a2e73" },
    { "deqc1",      0x500, "a6ba15477df0bc1e048fcd98a20ec317b46a42e94dd53a1a817030a6d21fe155" },
    { "deqm1",      0x500, "3a2ed94c344a45929bd2659f6d8255e10ee0a5c17eeae9a0ba918554303d84c5" },
    { "dspc1",      0x500, "16f0a76986df8cb603dfd49dc06e86b1b3e2a38ff81ec1100bdb08c5a7eb765e" },
    { "dspa1",      0x280, "3ec51ca6eb8d806c7b75cb2f94f5d36666ef865636d562ab4a1cd78710b483f9" },
    { "dspm1_0",    0x500, "631a277a63955afaba31bdf4c35278990fb7149391e933bf1b970213f29a3343" },
    { "dspm1_1",    0x500, "0fcedfa8a1c106744df036557398eeb6d64d6d85aace837808a0207d676eef54" },
    { "n1mod0",     0x100, "a071c84600d87be9153a6b800050d86d2877f768b9dbb9823fef4902f233fd8f" },
    { "n1creg0CMEL",0x18,  "7143ac1dab4981acf06c3889b17d064691dacd229b4acfc64ebf39ce58565ac2" },
    { "n1fgtim0",   0x80,  "98a4c64a9b450ca9079dfe72650227573009ee23d205d8421eed90d7a1aec89b" },
    { "n1fguram0",  0x100, "5341e6b2646979a70e57653007a1f310169421ec9bdd9f1a5648f75ade005af1" },
    { "n1creg0MEL", 0x18,  "a9f43fa50b3192ccf184639cb47de95f6b4eb6800ce1aabb1fc3206f7fad3cf6" },
    { "n1greg0",    0x20,  "558a2825bee764e76bc1870ebf1dab535fb8b565df26802f8afc42c45e196ca6" },
    { "n1greg1",    0x20,  "12b0ff10ef899ea80cac5cb747817fd6fa4200e9f6a9d381d96a4914d88339e9" },
    { "n1greg2",    0x20,  "12b0ff10ef899ea80cac5cb747817fd6fa4200e9f6a9d381d96a4914d88339e9" },
    { "n1greg3",    0x20,  "12b0ff10ef899ea80cac5cb747817fd6fa4200e9f6a9d381d96a4914d88339e9" },
    { "n1greg4",    0x20,  "12b0ff10ef899ea80cac5cb747817fd6fa4200e9f6a9d381d96a4914d88339e9" },
    { "prerec_306f0", 0x2c,"85759b3811ff7dc47b03792ac85317be51431a3f9e01dcafce317ed736a391b0" },
    { "prerec_226a0", 0x2c,"4b27d441db7331b18a68a7bc3882b8203fafbb88751a7bb7c0044bed544a4f0e" },
    { "prerec_226cc", 0x2c,"3d37a5f445e7d0037f1cc653b76b15b7efe14b1a6ba7aa1f2ae4b1ac229bdda4" },
    { "prerec_226f8", 0x2c,"2971ee05c37908c1e9115ae10a636af807a8d7666d36cad5e6d8e21e8ba979d2" },
    { "prerec_22724", 0x10,"3d0a787e15111027c66760ca16ee55036b5e5a13ff10ec0875d588e995e94667" },
    { "prerec_22734", 0x10,"ffe3f061c5912afd2b7bc99f3879b78bfed14cdddb89534a86d42ffeb16d4083" },
    { "playmode_dspc_234d0", 0x10,"2666f1a90cde51c85bf501d41f0199709097f77f41e30aafed429fbbd76a2933" },
    { "playmode_dspc_234e0", 0x10,"40231e534bdaef895ee99d550626875339acdbd2b0be38bea389c865a2f87788" },
    { "recmode_dspc_234f0",  0xc, "9126cc43e40fdd9f0ac1aaca87dfa559cdd3a8f8fab39884356593890e2ef662" },
    { "recmode_dspc_234fc",  0xc, "58f6982fed1846182d4218ce27d734e9adad0abe236b7bd7dee9e031d878d283" },
    { "playmode_addr_23508", 0x8, "9cc1f4c8cadb62105549134249d4b9d82fc63fbad4d844a7af78fb0b826901f9" },
    { "playmode_addr_23510", 0x8, "3e673b26d432ec0fec20d7a9c61ad9d36e22df26e5eb7ae3d534e7b6489fae54" },
    { "playmode_addr_23518", 0x8, "f4e10ade4808f99dac7318f14b54f44fa929e4cc733c09b11689a0e2adc1f5c1" },
    { "recmode_addr_23520",  0x4, "9af0754e59d25340c505ae76b3db7d852d791ffbc5d8d574365aa00dbbc394b5" },
    { "recmode_addr_23524",  0x4, "6c984eb4e413b3807efeebb5fd2ed29184557bd1aa9d5651302cb5dec67b0635" },
    { "recmode_addr_23528",  0x4, "6eb945bba3b5cd96e9a2f5b37defc373aee0756593c5fde0b22f07ebc90633d3" },
    { "recmode_addr_2352c",  0x4, "91d0acdd9440ee2e69531a266ca95ba8d415ff4f5a6f5a72c097df6cafcec89a" },
};

const SW_BLOB g_SwMprSlotBlob[SW_MPR_SLOT_COUNT] = {
    SW_BLOB_BCR1MEL, SW_BLOB_DEQC1, SW_BLOB_DEQM1, SW_BLOB_DSPC1, SW_BLOB_DSPA1,
    SW_BLOB_DSPM1_0, SW_BLOB_DSPM1_1, SW_BLOB_N1MOD0, SW_BLOB_N1CREG0CMEL,
    SW_BLOB_N1FGTIM0, SW_BLOB_N1FGURAM0,
};

/* mprSize in words (re/05 slot table); length bytes = words * 4. */
const ULONG g_SwMprSlotWords[SW_MPR_SLOT_COUNT] = {
    0x140, 0x140, 0x140, 0x140, 0xa0, 0x140, 0x140, 0x40, 0x6, 0x20, 0x40,
};

#define SW_SHA256_LEN 32

/* ---- loader ------------------------------------------------------------- */

CSw1000Microcode::~CSw1000Microcode()
{
    Unload();
}

void
CSw1000Microcode::Unload()
{
    if (m_data != nullptr) {
        RtlSecureZeroMemory(m_data, m_length);
        ExFreePoolWithTag(m_data, SW1000_POOLTAG);
        m_data = nullptr;
        m_length = 0;
    }
}

const UCHAR *
CSw1000Microcode::Blob(_In_ SW_BLOB id, _Out_opt_ PULONG lengthBytes) const
{
    if (m_data == nullptr || id >= SW_BLOB_COUNT) {
        if (lengthBytes) *lengthBytes = 0;
        return nullptr;
    }
    if (lengthBytes) *lengthBytes = g_SwBlobs[id].length;
    return m_data + m_offset[id];
}

const ULONG *
CSw1000Microcode::BlobWords(_In_ SW_BLOB id, _Out_opt_ PULONG wordCount) const
{
    ULONG bytes = 0;
    const UCHAR *p = Blob(id, &bytes);
    if (wordCount) *wordCount = bytes / sizeof(ULONG);
    return reinterpret_cast<const ULONG *>(p);
}

NTSTATUS
CSw1000Microcode::Load()
{
    PAGED_CODE();

    if (m_data != nullptr) {
        return STATUS_SUCCESS;
    }

    PUCHAR buffer = nullptr;
    ULONG  length = 0;
    NTSTATUS status = ReadRegistryBundle(&buffer, &length);
    if (!NT_SUCCESS(status)) {
        SwTrace("microcode: bundle not available (0x%08x)\n", status);
        return status;
    }

    m_data   = buffer;
    m_length = length;

    status = SliceAndCheck();
    if (NT_SUCCESS(status)) {
        status = VerifyHashes();
    }
    if (!NT_SUCCESS(status)) {
        Unload();
    }
    return status;
}

NTSTATUS
CSw1000Microcode::SliceAndCheck()
{
    ULONG expected = 0;
    for (ULONG i = 0; i < SW_BLOB_COUNT; ++i) {
        m_offset[i] = expected;
        expected   += g_SwBlobs[i].length;
    }
    if (m_length != expected) {
        SwTrace("microcode: bundle length 0x%x, expected 0x%x\n", m_length, expected);
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    return STATUS_SUCCESS;
}

/* Parse 64 lowercase/upper hex chars into 32 bytes. */
static BOOLEAN
HexToBytes(_In_ const char *hex, _Out_writes_(32) UCHAR *out)
{
    for (int i = 0; i < SW_SHA256_LEN; ++i) {
        UCHAR b = 0;
        for (int k = 0; k < 2; ++k) {
            char c = hex[i * 2 + k];
            UCHAR n;
            if (c >= '0' && c <= '9')      n = (UCHAR)(c - '0');
            else if (c >= 'a' && c <= 'f') n = (UCHAR)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') n = (UCHAR)(c - 'A' + 10);
            else return FALSE;
            b = (UCHAR)((b << 4) | n);
        }
        out[i] = b;
    }
    return TRUE;
}

NTSTATUS
CSw1000Microcode::VerifyHashes()
{
    PAGED_CODE();

    BCRYPT_ALG_HANDLE alg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM,
                                                  nullptr, 0);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    ULONG hashLen = 0, cb = 0;
    status = BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PUCHAR)&hashLen,
                               sizeof(hashLen), &cb, 0);
    if (!NT_SUCCESS(status) || hashLen != SW_SHA256_LEN) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return NT_SUCCESS(status) ? STATUS_UNSUCCESSFUL : status;
    }

    UCHAR digest[SW_SHA256_LEN];
    UCHAR expected[SW_SHA256_LEN];

    for (ULONG i = 0; i < SW_BLOB_COUNT; ++i) {
        BCRYPT_HASH_HANDLE hash = nullptr;
        status = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
        if (!NT_SUCCESS(status)) break;

        status = BCryptHashData(hash, m_data + m_offset[i], g_SwBlobs[i].length, 0);
        if (NT_SUCCESS(status)) {
            status = BCryptFinishHash(hash, digest, SW_SHA256_LEN, 0);
        }
        BCryptDestroyHash(hash);
        if (!NT_SUCCESS(status)) break;

        if (!HexToBytes(g_SwBlobs[i].sha256hex, expected) ||
            RtlCompareMemory(digest, expected, SW_SHA256_LEN) != SW_SHA256_LEN) {
            SwTrace("microcode: hash mismatch on blob %u (%s)\n", i, g_SwBlobs[i].name);
            status = STATUS_IMAGE_CHECKSUM_MISMATCH;
            break;
        }
    }

    BCryptCloseAlgorithmProvider(alg, 0);
    return status;
}

/*
 * Read the "Microcode" REG_BINARY from <service key>\Parameters. The installer
 * (install/Install-Microcode.ps1) writes the bundle there after extracting it
 * from the user's yswds.sys.
 */
NTSTATUS
CSw1000Microcode::ReadRegistryBundle(_Outptr_ PUCHAR *buffer, _Out_ PULONG length)
{
    PAGED_CODE();

    *buffer = nullptr;
    *length = 0;

    HANDLE key = nullptr;
    NTSTATUS status = SwOpenParametersKey(KEY_READ, FALSE, &key);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    UNICODE_STRING valueName;
    RtlInitUnicodeString(&valueName, L"Microcode");

    /* First query the size. */
    ULONG resultLen = 0;
    status = ZwQueryValueKey(key, &valueName, KeyValuePartialInformation,
                             nullptr, 0, &resultLen);
    if (status != STATUS_BUFFER_TOO_SMALL && status != STATUS_BUFFER_OVERFLOW) {
        ZwClose(key);
        return NT_SUCCESS(status) ? STATUS_UNSUCCESSFUL : status;
    }

    PKEY_VALUE_PARTIAL_INFORMATION info =
        (PKEY_VALUE_PARTIAL_INFORMATION)ExAllocatePool2(POOL_FLAG_PAGED, resultLen, SW1000_POOLTAG);
    if (info == nullptr) {
        ZwClose(key);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = ZwQueryValueKey(key, &valueName, KeyValuePartialInformation,
                             info, resultLen, &resultLen);
    ZwClose(key);

    if (NT_SUCCESS(status)) {
        if (info->Type != REG_BINARY || info->DataLength == 0) {
            status = STATUS_OBJECT_TYPE_MISMATCH;
        } else {
            PUCHAR copy = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, info->DataLength, SW1000_POOLTAG);
            if (copy == nullptr) {
                status = STATUS_INSUFFICIENT_RESOURCES;
            } else {
                RtlCopyMemory(copy, info->Data, info->DataLength);
                *buffer = copy;
                *length = info->DataLength;
            }
        }
    }

    ExFreePoolWithTag(info, SW1000_POOLTAG);
    return status;
}
