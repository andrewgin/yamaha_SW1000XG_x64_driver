/*
 * sw1000_regs.h - SW1000XG register map and MMIO accessors.
 *
 * Every offset is relative to the single memory BAR.
 * Registers are 32-bit except the byte-wide 8251 UART block.
 *
 * Clean-room: our own description of the hardware interface, not Yamaha code.
 */

#ifndef SW1000_REGS_H
#define SW1000_REGS_H

#include "common.h"

/* Highest register touched is PORT1 at 0x3ff10; the BAR is at least 256 KB
 * (re/04). Real size is U-006 (needs-hw); we require at least this much. */
#define SW_BAR_MIN_LENGTH       0x40000u

/* ---- UART (byte-wide, 8251-style), two channels (re/04 UART block) ------- */
#define SW_REG_UART0_DATA       0x3e000u
#define SW_REG_UART0_CMD        0x3e001u    /* W: command   R: status */
#define SW_REG_UART1_DATA       0x3e002u
#define SW_REG_UART1_CMD        0x3e003u

#define SW_UART_CHANNELS        2u          /* 0: MIDI connector, 1: XG synth (re/10) */

/* 8251 command bits (re/04 UART table) */
#define SW_UART_CMD_TXEN        0x01u
#define SW_UART_CMD_RXEN        0x04u
#define SW_UART_CMD_ERR_RESET   0x10u
#define SW_UART_CMD_INT_RESET   0x40u
#define SW_UART_MODE_MIDI       0x4Eu       /* async x16, 8 data, no parity, 1 stop */

/* 8251 status bits */
#define SW_UART_ST_TXRDY        0x01u
#define SW_UART_ST_RXRDY        0x02u
#define SW_UART_ST_TXEMPTY      0x04u
#define SW_UART_ST_ERRORS       0x38u       /* parity | overrun | framing */

#define SW_REG_MKS              0x3e004u    /* 32-bit, opaque */

/* ---- DSP host ports 0..4: base + 0x100*n -------------------------------- */
#define SW_DSP_PORT_BASE        0x3f000u
#define SW_DSP_PORT_STRIDE      0x100u
#define SW_DSP_PORT_COUNT       5u          /* register blocks in the BAR */
#define SW_DSP_PORTS_PRESENT    1u          /* SW1000XG: DSP 0 only; 1-4 read all-ones (DS2416) */
#define SW_DSP_OFF_DATA         0x00u       /* DATA[i] = DATA + 4*i, i = 0..31 */
#define SW_DSP_OFF_CTRL         0x80u
#define SW_DSP_OFF_ADDR         0x84u
#define SW_DSP_OFF_GREG0        0x88u       /* used from M2 (re/05) */
#define SW_DSP_OFF_GREG1        0x8cu
#define SW_DSP_OFF_GREG2        0x90u
#define SW_DSP_DATA_WORDS       32u

/* CTRL (base+0x80): bit31 BUSY (R); bit24 data-phase flag (W, U-007);
 * bits23:16 word count (W); bits15:0 = address bits 31:16 (W). */
#define SW_DSP_CTRL_BUSY        0x80000000u
#define SW_DSP_CTRL_FLAG        0x01000000u
#define SW_DSP_CTRL_COUNT1      0x00010000u /* one word in the count field */

/* ADDR (base+0x84): bits31:16 = address bits 15:0; bits15:0 = selector. */

/* port-0 extras */
#define SW_REG_PEAK             0x3f094u
#define SW_REG_ASIOPOS          0x3f098u    /* Phase 2 */
#define SW_REG_SCR              0x3f09cu

/* ---- control block ------------------------------------------------------ */
#define SW_REG_PORT0            0x3ff00u    /* board config (R) / control (W) */
#define SW_REG_TRPIF            0x3ff04u    /* irq enable (W) / status (R) */

/* TRPIF source bits (re/04, re/06). Hardware round 2: the status read shows a
 * UART's TX-ready condition even while its enable bit is clear, so the ISR
 * masks the status with the enable shadow. */
#define SW_TRPIF_UART0_TX       0x01000000u /* bit 24 */
#define SW_TRPIF_UART0_RX       0x02000000u /* bit 25 */
#define SW_TRPIF_UART1_TX       0x04000000u /* bit 26: SWXG1-3 to the synth */
#define SW_TRPIF_UART1_RX       0x08000000u /* bit 27: unused on SW1000 */
#define SW_REG_PORT1            0x3ff10u    /* global control (W, shadowed) */

/* ---- PORT1 bits (re/04) ------------------------------------------------- */
#define SW_PORT1_IRQEN          0x80000000u /* bit 31 master interrupt enable */
#define SW_PORT1_RUN            0x40000000u /* bit 30 transfer-engine run */
#define SW_PORT1_CCK            0x20000000u /* bit 29 DIT serial clock */
#define SW_PORT1_CLDW           0x10000000u /* bit 28 DIT latch/word strobe */
#define SW_PORT1_CDO0           0x08000000u /* bit 27 DIT serial data */
#define SW_PORT1_MUTE           0x02000000u /* bit 25 output mute */
#define SW_PORT1_NRES_DSP0      0x00800000u /* bit 23 DSP0 reset release */
#define SW_PORT1_FS_MASK        0x00030000u /* bits 17:16 sample rate */
#define SW_PORT1_FS_48000       0x00000000u
#define SW_PORT1_FS_44100       0x00010000u /* 01 and 11 both decode 44100 */
#define SW_PORT1_FS_32000       0x00020000u

/* ---- host-port selectors (re/04) ---------------------------------------- */
#define SW_SEL_DSP_PARAM        0x100u      /* DSP parameter/register space */
#define SW_SEL_MOD              0x700u      /* mod area (slot 7): key / run word */
#define SW_SEL_CESCR            0x800u      /* CES control registers */
#define SW_SEL_RAM_READ         0xe00u      /* "RAM" read */
#define SW_SEL_RAM_WRITE        0xf00u      /* "RAM" write */

/* ---- DSP run / key constants (re/05) ------------------------------------ */
#define SW_MOD_KEY_ADDR         0x40u       /* key word address in the mod area */
#define SW_MOD_KEY_ON           0x0000ffffu /* written before the MPR load */
#define SW_MOD_KEY_OFF          0x00000000u /* written after the MPR load */
/* Run word. The original's dspSetRun picks one of two paths on the "32-bit
 * MPR" flag, which is always set, so only the 32-bit form is ever used; the
 * 16-bit path (SetRun16) is deliberately not reproduced (re/05). */
#define SW_DSP_RUN_ADDR_32      0xe0u       /* parameter-space run word */
#define SW_DSP_RUN_VALUE        0x40000000u /* run; 0 = stop */

/* ---- DIT (S/PDIF transmitter) serial interface on PORT1 (re/05) --------- *
 * One register write: CLDW low; shift out the data bits (order per register)
 * then two select bits = register number, MSB first; CLDW high. A bit is sent
 * as CDO0 <- bit, CCK high, CCK low. */
#define SW_DIT_REG_COUNT        3u

/* ---- "RAM" stream-register file addresses (port 0, re/04) --------------- */
#define SW_RAM_SA_BASE          0xc000u     /* + channel */
#define SW_RAM_EA_BASE          0xc020u
#define SW_RAM_IA_BASE          0xc040u
#define SW_RAM_TP_BASE          0xc060u
#define SW_RAM_TRWF             0xc100u     /* global transfer-enable mask */
#define SW_RAM_TRWFO            0xc101u     /* transfer-active feedback */
#define SW_RAM_SUSF             0xc102u

/* ---- MMIO accessors ----------------------------------------------------- */
__forceinline ULONG Sw_Rd32(_In_ PVOID bar, _In_ ULONG off)
{ return READ_REGISTER_ULONG((PULONG)((PUCHAR)bar + off)); }

__forceinline void Sw_Wr32(_In_ PVOID bar, _In_ ULONG off, _In_ ULONG v)
{ WRITE_REGISTER_ULONG((PULONG)((PUCHAR)bar + off), v); }

__forceinline UCHAR Sw_Rd8(_In_ PVOID bar, _In_ ULONG off)
{ return READ_REGISTER_UCHAR((PUCHAR)bar + off); }

__forceinline void Sw_Wr8(_In_ PVOID bar, _In_ ULONG off, _In_ UCHAR v)
{ WRITE_REGISTER_UCHAR((PUCHAR)bar + off, v); }

__forceinline ULONG Sw_DspPort(_In_ ULONG n)
{ return SW_DSP_PORT_BASE + SW_DSP_PORT_STRIDE * n; }

#endif /* SW1000_REGS_H */
