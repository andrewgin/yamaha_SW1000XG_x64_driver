/*
 * structs.h - descriptive layout of the recovered yswds.sys classes/structs.
 *
 * CLEAN-ROOM NOTE. This header is OUR OWN description of the object layouts
 * recovered by static analysis. It is written from the
 * facts (offsets, sizes, element counts), not copied from any decompiler output,
 * and it is for reference while planning the x64 port. The port defines its own
 * types; it does not include this file.
 *
 * All offsets are confirmed from the constructors and CAdapterCommon::Init
 * unless a comment says otherwise. "untraced" = the field is written but its use
 * was not followed; it is not needed for the Phase-1 port.
 *
 * x86 (original) assumptions: pointers are 4 bytes, KSPIN_LOCK is 4 bytes,
 * KIRQL is 1 byte. On x64 these widths change, so this layout describes the
 * ORIGINAL binary only.
 */

#ifndef YSWDS_STRUCTS_H
#define YSWDS_STRUCTS_H

#include <stdint.h>

/* Forward references to Windows / PortCls types (opaque here). */
typedef void IAdapterCommon;
typedef void IAdapterPowerManagement;
typedef void CUnknown_vtbl;
typedef void IInterruptSync;
typedef void IPortMidi;
typedef void IPortWaveCyclic;
typedef void IServiceGroup;
typedef void DEVICE_OBJECT;

/*
 * Yamaha's 12-byte spin-lock wrapper.
 * The ctor sets `self` to &lock and calls KeInitializeSpinLock(&lock).
 * KSpinLock::Lock(atDpc) raises to DISPATCH (every caller passes atDpc=FALSE).
 */
typedef struct _YSW_SPINLOCK {       /* size 0x0c */
    void     *self;                  /* +0x00 PKSPIN_LOCK, points at `lock` */
    uint32_t  lock;                  /* +0x04 KSPIN_LOCK */
    uint8_t   saved_irql;            /* +0x08 KIRQL saved across the hold */
    uint8_t   pad[3];                /* +0x09 */
} YSW_SPINLOCK;

/*
 * Per-hardware-channel record, built by InitChannelInfo (0x3388e).
 * 16 of these for playback (bit = index) and 8 for capture (bit = index+16).
 * Only the first (2 x subdevice-count) are marked active; the rest are zeroed.
 * Fields past +0x24 are left uninitialised by InitChannelInfo and are filled by
 * the streaming paths.
 */
typedef struct _YSW_CHANNEL_REC {    /* size 0x44 */
    uint32_t active;                 /* +0x00 1 = active, 0 = unused */
    uint32_t index;                  /* +0x04 channel index */
    uint32_t irq_bit;                /* +0x08 interrupt bit number */
    uint32_t irq_mask;               /* +0x0c 1u << irq_bit */
    uint32_t f10;                    /* +0x10 = 0 at init */
    uint32_t f14;                    /* +0x14 = 0 at init */
    int32_t  f18;                    /* +0x18 = -1 at init */
    uint32_t f1c;                    /* +0x1c = 0 at init */
    uint32_t f20;                    /* +0x20 = 0 at init */
    int32_t  f24;                    /* +0x24 = 0x7fffffff at init */
    uint8_t  stream_state[0x1c];     /* +0x28 filled by streaming paths */
} YSW_CHANNEL_REC;

/*
 * CAdapterCommon (size 0x2500). The shared hardware layer; CAdapterSW1000 and
 * CAdapterDS2416 embed/derive from it. Three interface vptrs lead the object.
 * Reserved[] arrays bridge gaps so the named members land on their real offsets.
 */
typedef struct _CADAPTERCOMMON {                    /* size 0x2500 */
    void      *vptr_IAdapterCommon;                 /* +0x000 */
    void      *vptr_IAdapterPowerMgmt;              /* +0x004 */
    void      *vptr_CUnknown;                       /* +0x008 */
    uint8_t    rsvd_00c[0x08];                      /* +0x00c CUnknown base interior */
    uint32_t   f014;                                /* +0x014 untraced (zeroed) */
    uint32_t   f018;                                /* +0x018 untraced */
    uint32_t   asio_run_flag;                       /* +0x01c default 1; set by initAsio */
    uint32_t   f020;                                /* +0x020 untraced */
    uint8_t    f024;                                /* +0x024 untraced */
    uint8_t    mixer_count;                         /* +0x025 mixer settings count (28) */
    uint8_t    rsvd_026[2];                         /* +0x026 */
    uint32_t   f028;                                /* +0x028 untraced */
    void      *mixer_cache;                         /* +0x02c -> derived mixer value cache */
    YSW_SPINLOCK lock_a;                            /* +0x030 untraced (ASIO candidate) */
    YSW_SPINLOCK lock_b;                            /* +0x03c untraced (ASIO candidate) */
    YSW_SPINLOCK lock_hostport[6];                  /* +0x048 host ports 0..5 */
    YSW_SPINLOCK lock_mks;                          /* +0x090 MKS register */
    YSW_SPINLOCK lock_port0;                        /* +0x09c PORT0 + shadow f174 */
    YSW_SPINLOCK lock_port1;                        /* +0x0a8 PORT1 + shadow f180 */
    YSW_SPINLOCK lock_trpif;                        /* +0x0b4 TRPIF + shadow f178 */
    void      *default_mixer_table;                 /* +0x0c0 -> default mixer settings */
    DEVICE_OBJECT *device_object;                   /* +0x0c4 */
    IInterruptSync *interrupt_sync;                 /* +0x0c8 */
    IPortMidi      *midi_port[5];                   /* +0x0cc per MIDI subdevice */
    IPortWaveCyclic *wave_port[12];                 /* +0x0e0 per wave subdevice (8 used) */
    IServiceGroup  *midi_service_group[5];          /* +0x110 */
    IServiceGroup  *wave_service_group[12];         /* +0x124 (8 used) */
    uint32_t   bar_length;                          /* +0x154 register window length */
    uint32_t   f158;                                /* +0x158 untraced */
    uint32_t   f15c;                                /* +0x15c untraced */
    uint16_t   f160;                                /* +0x160 untraced (ushort) */
    uint16_t   rsvd_162;                            /* +0x162 */
    void      *reg_base;                            /* +0x164 mapped register base (non-cached) */
    uint32_t   f168;                                /* +0x168 register state, untraced */
    uint32_t   f16c;                                /* +0x16c register state, untraced */
    uint32_t   f170;                                /* +0x170 register state, untraced */
    uint32_t   shadow_port0;                        /* +0x174 PORT0 shadow */
    uint32_t   shadow_trpif;                        /* +0x178 TRPIF shadow */
    uint32_t   f17c;                                /* +0x17c register state, untraced */
    uint32_t   shadow_port1;                        /* +0x180 PORT1 shadow */
    uint32_t   f184;                                /* +0x184 register state, untraced */
    uint32_t   power_change_enable;                 /* +0x188 1 = may change D-state */
    uint8_t    board_flag;                          /* +0x18c 1 on SW1000 */
    uint8_t    f18d;                                /* +0x18d untraced */
    uint8_t    f18e;                                /* +0x18e untraced */
    uint8_t    wave_in_count;                       /* +0x18f 2 on SW1000 */
    uint8_t    wave_out_count;                      /* +0x190 6 on SW1000 */
    uint8_t    f191;                                /* +0x191 untraced */
    uint8_t    f192;                                /* +0x192 untraced */
    uint8_t    rsvd_193;                            /* +0x193 */
    uint32_t   device_power_state;                  /* +0x194 1=D0 .. 4=D3 */
    YSW_CHANNEL_REC capture_ch[8];                  /* +0x198 capture (4 active) */
    YSW_CHANNEL_REC playback_ch[16];                /* +0x3b8 playback (12 active) */
    uint32_t   f7f8;                                /* +0x7f8 untraced */
    uint32_t   f7fc;                                /* +0x7fc untraced */
    uint8_t    asio_swds[0x1b80];                   /* +0x800 embedded CAsioSWDS (IRQ ptr @ +0x48) */
    uint8_t    rsvd_2380[0x0c0];                    /* +0x2380 zeroed region, untraced */
    uint8_t    rsvd_2440[0x060];                    /* +0x2440 zeroed region, untraced */
    uint8_t    rsvd_24a0[0x060];                    /* +0x24a0 zeroed region, untraced (to 0x2500) */
} CADAPTERCOMMON;

/*
 * CAdapterSW1000 (size 0x6b40, from NewAdapterSW1000). Multiple inheritance:
 * IMidiDevice leads at +0, then an embedded CPciUart, the CAdapterCommon above,
 * the MIDI objects, and the mixer cache the CAdapterCommon.mixer_cache points at.
 */
typedef struct _CADAPTERSW1000 {                    /* size 0x6b40 */
    void          *vptr_IMidiDevice;                /* +0x0000 */
    uint8_t        pci_uart[0x40c4];                /* +0x0004 embedded CPciUart */
    CADAPTERCOMMON common;                          /* +0x40c8 */
    void          *uart_bar_base;                   /* +0x65c8 second BAR mapping (CC+0x2500) */
    uint32_t       uart_bar_length;                 /* +0x65cc */
    uint8_t        rsvd_65d0[0x08];                 /* +0x65d0 */
    uint8_t        midi_in[0x450];                  /* +0x65d8 CMidiIn */
    uint8_t        midi_out[4][0x28];               /* +0x6a28 CMidiOut[4] */
    uint32_t       mixer_value_cache[28];           /* +0x6ac8 seeded ~default */
    uint32_t       f6b38;                           /* +0x6b38 untraced */
    uint8_t        rsvd_6b3c[0x04];                 /* +0x6b3c */
} CADAPTERSW1000;

/*
 * Self-check: these offsets describe the 32-bit original, so the asserts only
 * fire on a 4-byte-pointer build (e.g. `gcc -m32`). On a 64-bit build pointers
 * widen and the layout no longer matches, so the block is skipped by design.
 */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 4
#include <stddef.h>
#define YSW_AT(f, o) _Static_assert(offsetof(CADAPTERCOMMON, f) == (o), #f)
YSW_AT(asio_run_flag, 0x1c);  YSW_AT(mixer_cache, 0x2c);
YSW_AT(lock_hostport, 0x48);  YSW_AT(lock_mks, 0x90);
YSW_AT(lock_port0, 0x9c);     YSW_AT(lock_port1, 0xa8);  YSW_AT(lock_trpif, 0xb4);
YSW_AT(default_mixer_table, 0xc0); YSW_AT(device_object, 0xc4); YSW_AT(interrupt_sync, 0xc8);
YSW_AT(midi_port, 0xcc);      YSW_AT(wave_port, 0xe0);
YSW_AT(midi_service_group, 0x110); YSW_AT(wave_service_group, 0x124);
YSW_AT(bar_length, 0x154);    YSW_AT(reg_base, 0x164);
YSW_AT(shadow_port0, 0x174);  YSW_AT(shadow_trpif, 0x178); YSW_AT(shadow_port1, 0x180);
YSW_AT(power_change_enable, 0x188); YSW_AT(device_power_state, 0x194);
YSW_AT(capture_ch, 0x198);    YSW_AT(playback_ch, 0x3b8); YSW_AT(asio_swds, 0x800);
#undef YSW_AT
_Static_assert(sizeof(YSW_SPINLOCK) == 0x0c, "YSW_SPINLOCK size");
_Static_assert(sizeof(YSW_CHANNEL_REC) == 0x44, "YSW_CHANNEL_REC size");
_Static_assert(sizeof(CADAPTERCOMMON) == 0x2500, "CADAPTERCOMMON size");
_Static_assert(offsetof(CADAPTERSW1000, common) == 0x40c8, "CADAPTERSW1000.common");
_Static_assert(offsetof(CADAPTERSW1000, mixer_value_cache) == 0x6ac8, "CADAPTERSW1000.mixer");
_Static_assert(sizeof(CADAPTERSW1000) == 0x6b40, "CADAPTERSW1000 size");
#endif

#endif /* YSWDS_STRUCTS_H */
