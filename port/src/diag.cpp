/*
 * diag.cpp - bring-up diagnostics persisted to the registry (see diag.h).
 *
 * The log is plain ASCII built in memory, mirrored to DbgPrintEx (visible in
 * DebugView with verbose kernel output), and written as REG_SZ DiagLog at
 * every stage edge. Before a stage runs, DiagBreadcrumb is written and the key
 * flushed to disk, so a crash inside the stage leaves the breadcrumb (and the
 * log up to that point) for the next start to find.
 */

#include "diag.h"
#include <ntstrsafe.h>

#if __has_include("buildid.h")
#include "buildid.h"                /* written by CI: ref, commit, run number */
#endif
#ifndef SW_BUILD_ID
#define SW_BUILD_ID "local build"
#endif

#pragma code_seg("PAGE")

static const char *const g_StageNames[SW_STAGE_END] = {
    "none", "identify", "map", "probe", "microcode", "coldstart",
    "uart", "irq", "midiports",
};

const char *
CSwDiag::StageName(_In_ ULONG stage)
{
    return stage < (ULONG)SW_STAGE_END ? g_StageNames[stage] : "unknown";
}

CSwDiag::~CSwDiag()
{
    PAGED_CODE();
    if (m_key != nullptr) {
        ZwClose(m_key);
    }
}

void
CSwDiag::Log(_In_z_ _Printf_format_string_ const char *format, ...)
{
    PAGED_CODE();

    /* "  1234.567  text": milliseconds since StartSession. */
    char line[256];
    char *end = line;
    size_t remaining = sizeof(line);
    ULONGLONG t = SwNow100ns() - (ULONGLONG)m_t0;
    RtlStringCbPrintfExA(line, sizeof(line), &end, &remaining, 0, "%7lu.%03lu  ",
                         (ULONG)(t / 10000), (ULONG)((t / 10) % 1000));

    va_list args;
    va_start(args, format);
    RtlStringCbVPrintfExA(end, remaining, &end, &remaining, 0, format, args);  /* truncates */
    va_end(args);

    DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_INFO_LEVEL, "ysw1000xg: %s\n", line);

    static const char kTruncated[] = "[log full]\r\n";
    size_t len = (size_t)(end - line);
    if (m_used + len + 2 > sizeof(m_log) - sizeof(kTruncated)) {
        if (!m_truncated) {
            RtlCopyMemory(m_log + m_used, kTruncated, sizeof(kTruncated) - 1);
            m_used += sizeof(kTruncated) - 1;
            m_truncated = TRUE;
        }
        return;
    }
    RtlCopyMemory(m_log + m_used, line, len);
    m_used += (ULONG)len;
    m_log[m_used++] = '\r';
    m_log[m_used++] = '\n';
}

/* ---- registry helpers (all no-ops without the key) ----------------------- */

BOOLEAN
CSwDiag::ReadDword(_In_z_ PCWSTR name, _Out_ PULONG value)
{
    PAGED_CODE();
    *value = 0;
    if (m_key == nullptr) {
        return FALSE;
    }
    UNICODE_STRING n;
    RtlInitUnicodeString(&n, name);
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    ULONG got = 0;
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    if (!NT_SUCCESS(ZwQueryValueKey(m_key, &n, KeyValuePartialInformation, info, sizeof(buf), &got)) ||
        info->Type != REG_DWORD || info->DataLength != sizeof(ULONG)) {
        return FALSE;
    }
    RtlCopyMemory(value, info->Data, sizeof(ULONG));
    return TRUE;
}

void
CSwDiag::WriteDword(_In_z_ PCWSTR name, _In_ ULONG value)
{
    PAGED_CODE();
    if (m_key == nullptr) {
        return;
    }
    UNICODE_STRING n;
    RtlInitUnicodeString(&n, name);
    ZwSetValueKey(m_key, &n, 0, REG_DWORD, &value, sizeof(value));
}

void
CSwDiag::DeleteValue(_In_z_ PCWSTR name)
{
    PAGED_CODE();
    if (m_key == nullptr) {
        return;
    }
    UNICODE_STRING n;
    RtlInitUnicodeString(&n, name);
    ZwDeleteValueKey(m_key, &n);
}

void
CSwDiag::CopyValue(_In_z_ PCWSTR from, _In_z_ PCWSTR to)
{
    PAGED_CODE();
    if (m_key == nullptr) {
        return;
    }
    UNICODE_STRING n;
    RtlInitUnicodeString(&n, from);
    ULONG size = 0;
    NTSTATUS status = ZwQueryValueKey(m_key, &n, KeyValuePartialInformation, nullptr, 0, &size);
    if (status != STATUS_BUFFER_TOO_SMALL && status != STATUS_BUFFER_OVERFLOW) {
        return;
    }
    PKEY_VALUE_PARTIAL_INFORMATION info =
        (PKEY_VALUE_PARTIAL_INFORMATION)ExAllocatePool2(POOL_FLAG_PAGED, size, SW1000_POOLTAG);
    if (info == nullptr) {
        return;
    }
    if (NT_SUCCESS(ZwQueryValueKey(m_key, &n, KeyValuePartialInformation, info, size, &size))) {
        UNICODE_STRING t;
        RtlInitUnicodeString(&t, to);
        ZwSetValueKey(m_key, &t, 0, info->Type, info->Data, info->DataLength);
    }
    ExFreePoolWithTag(info, SW1000_POOLTAG);
}

void
CSwDiag::PersistLog()
{
    PAGED_CODE();
    if (m_key == nullptr) {
        return;
    }
    ULONG bytes = (m_used + 1) * sizeof(WCHAR);
    PWCHAR wide = (PWCHAR)ExAllocatePool2(POOL_FLAG_PAGED, bytes, SW1000_POOLTAG);
    if (wide == nullptr) {
        return;
    }
    for (ULONG i = 0; i < m_used; ++i) {
        wide[i] = (WCHAR)(UCHAR)m_log[i];      /* the log is ASCII */
    }
    wide[m_used] = L'\0';
    UNICODE_STRING n;
    RtlInitUnicodeString(&n, L"DiagLog");
    ZwSetValueKey(m_key, &n, 0, REG_SZ, wide, bytes);
    ExFreePoolWithTag(wide, SW1000_POOLTAG);
}

/* ---- session and stages --------------------------------------------------- */

void
CSwDiag::StartSession()
{
    PAGED_CODE();

    m_t0 = (LONGLONG)SwNow100ns();
    NTSTATUS status = SwOpenParametersKey(KEY_READ | KEY_WRITE, TRUE, &m_key);
    if (!NT_SUCCESS(status)) {
        m_key = nullptr;
    }

    Log("ysw1000xg diagnostics; build %s", SW_BUILD_ID);
    if (m_key == nullptr) {
        Log("registry: cannot open Parameters (0x%08x); this log is not saved", status);
    }

    ULONG count = 0;
    ReadDword(L"DiagStartCount", &count);
    WriteDword(L"DiagStartCount", ++count);
    WriteDword(L"DiagStageCount", SW_STAGE_LAST);

    ULONG value = 0;
    if (ReadDword(L"DiagMaxStage", &value)) {
        m_maxStage = value;
    }
    if (ReadDword(L"DiagSelfTest", &value)) {
        m_selfTest = (value != 0);
        DeleteValue(L"DiagSelfTest");          /* one start only */
    }

    if (ReadDword(L"DiagBreadcrumb", &value)) {
        CopyValue(L"DiagLog", L"DiagCrashLog");
        WriteDword(L"DiagCrashedStage", value);
        DeleteValue(L"DiagBreadcrumb");
        Log("previous start never finished stage %lu (%s): it crashed, hung or lost power there",
            value, StageName(value));
    }
    if (ReadDword(L"DiagCrashedStage", &value) && value != 0) {
        m_crashedStage = value;
        Log("crash marker set (stage %lu %s): stages that touch the card stay off until "
            "Run-HwTest.ps1 clears it", value, StageName(value));
    }

    char stages[160] = "";
    for (ULONG s = 1; s <= SW_STAGE_LAST; ++s) {
        char item[24];
        RtlStringCbPrintfA(item, sizeof(item), "%s%lu %s", s == 1 ? "" : ", ", s, StageName(s));
        RtlStringCbCatA(stages, sizeof(stages), item);
    }
    Log("start %lu; DiagMaxStage %lu; self-tests %s; stages: %s",
        count, m_maxStage, m_selfTest ? "on" : "off", stages);
    PersistLog();
}

void
CSwDiag::EndSession(_In_ NTSTATUS status)
{
    PAGED_CODE();
    Log("RESULT status=0x%08x completed=%lu/%lu (%s)",
        status, m_lastCompleted, SW_STAGE_LAST, StageName(m_lastCompleted));
    Log("END");
    PersistLog();
    if (m_key != nullptr) {
        ZwClose(m_key);
        m_key = nullptr;
    }
}

BOOLEAN
CSwDiag::BeginStage(_In_ SW_STAGE stage)
{
    PAGED_CODE();

    if ((ULONG)stage > m_maxStage) {
        Log("stage %lu %s: not run (DiagMaxStage %lu)", (ULONG)stage, StageName(stage), m_maxStage);
        m_stopStatus = STATUS_SUCCESS;
        return FALSE;
    }
    if (m_crashedStage != 0 && stage >= SW_STAGE_MAP) {
        Log("stage %lu %s: refused (crash marker)", (ULONG)stage, StageName(stage));
        m_stopStatus = STATUS_DEVICE_NOT_READY;
        return FALSE;
    }

    m_stage = stage;
    m_stageT0 = (LONGLONG)SwNow100ns();
    Log("stage %lu %s: begin", (ULONG)stage, StageName(stage));
    WriteDword(L"DiagBreadcrumb", (ULONG)stage);
    PersistLog();
    if (m_key != nullptr) {
        ZwFlushKey(m_key);                  /* breadcrumb on disk before the stage */
    }
    return TRUE;
}

void
CSwDiag::EndStage(_In_ NTSTATUS status)
{
    PAGED_CODE();
    ULONG us = (ULONG)(((LONGLONG)SwNow100ns() - m_stageT0) / 10);
    Log("stage %lu %s: %s (0x%08x) in %lu us", (ULONG)m_stage, StageName(m_stage),
        NT_SUCCESS(status) ? "ok" : "FAILED", status, us);
    DeleteValue(L"DiagBreadcrumb");
    if (NT_SUCCESS(status)) {
        m_lastCompleted = (ULONG)m_stage;
    }
    m_stage = SW_STAGE_NONE;
    PersistLog();
}

#pragma code_seg()
