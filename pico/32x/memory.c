/*
 * PicoDrive
 * (C) notaz, 2009,2010
 *
 * This work is licensed under the terms of MAME license.
 * See COPYING file in the top-level directory.
 *
 * SH2 addr lines:
 * iii. .cc. ..xx *   // Internal, Cs, x
 *
 * Register map:
 * a15100 F....... R.....EA  F.....AC N...VHMP 4000 // Fm Ren nrEs Aden Cart heN V H cMd Pwm
 * a15102 ........ ......SM  ?                 4002 // intS intM
 * a15104 ........ ......10  ........ hhhhhhhh 4004 // bk1 bk0 Hint
 * a15106 F....... .....SDR  UE...... .....SDR 4006 // Full 68S Dma Rv fUll[fb] Empt[fb]
 * a15108           (32bit DREQ src)           4008
 * a1510c           (32bit DREQ dst)           400c
 * a15110          llllllll llllll00           4010 // DREQ Len
 * a15112           (16bit FIFO reg)           4012
 * a15114 ?                  (16bit VRES clr)  4014
 * a15116 ?                  (16bit Vint clr)  4016
 * a15118 ?                  (16bit Hint clr)  4018
 * a1511a ........ .......C  (16bit CMD clr)   401a // Cm
 * a1511c ?                  (16bit PWM clr)   401c
 * a1511e ?                  ?                 401e
 * a15120            (16 bytes comm)           2020
 * a15130                 (PWM)                2030
 */
#include "../pico_int.h"
#include "../memory.h"
#ifdef DRC_SH2
#include "../../cpu/sh2/compiler.h"
#endif

#if 0
#undef ash2_end_run
#undef SekEndRun
#define ash2_end_run(x)
#define SekEndRun(x)
#endif

static const char str_mars[] = "MARS";

void *p32x_bios_g, *p32x_bios_m, *p32x_bios_s;
struct Pico32xMem *Pico32xMem;

static void bank_switch(int b);

// poll detection
#define POLL_THRESHOLD 6

struct poll_det {
  u32 addr, cycles, cyc_max;
  int cnt, flag;
};
static struct poll_det m68k_poll, sh2_poll[2];

static int p32x_poll_detect(struct poll_det *pd, u32 a, u32 cycles, int is_vdp)
{
  int ret = 0, flag = pd->flag;

  if (is_vdp)
    flag <<= 3;

  if (a - 2 <= pd->addr && pd->addr <= a + 2 && cycles - pd->cycles <= pd->cyc_max) {
    pd->cnt++;
    if (pd->cnt > POLL_THRESHOLD) {
      if (!(Pico32x.emu_flags & flag)) {
        elprintf(EL_32X, "%s poll addr %08x, cyc %u",
          flag & (P32XF_68KPOLL|P32XF_68KVPOLL) ? "m68k" :
          (flag & (P32XF_MSH2POLL|P32XF_MSH2VPOLL) ? "msh2" : "ssh2"), a, cycles - pd->cycles);
        ret = 1;
      }
      Pico32x.emu_flags |= flag;
    }
  }
  else {
    pd->cnt = 0;
    pd->addr = a;
  }
  pd->cycles = cycles;

  return ret;
}

static int p32x_poll_undetect(struct poll_det *pd, int is_vdp)
{
  int ret = 0, flag = pd->flag;
  if (is_vdp)
    flag <<= 3; // VDP only
  else
    flag |= flag << 3; // both
  if (Pico32x.emu_flags & flag) {
    elprintf(EL_32X, "poll %02x -> %02x", Pico32x.emu_flags, Pico32x.emu_flags & ~flag);
    ret = 1;
  }
  Pico32x.emu_flags &= ~flag;
  pd->addr = pd->cnt = 0;
  return ret;
}

void p32x_poll_event(int cpu_mask, int is_vdp)
{
  if (cpu_mask & 1)
    p32x_poll_undetect(&sh2_poll[0], is_vdp);
  if (cpu_mask & 2)
    p32x_poll_undetect(&sh2_poll[1], is_vdp);
}

// SH2 faking
//#define FAKE_SH2
int p32x_csum_faked;
#ifdef FAKE_SH2
static const u16 comm_fakevals[] = {
  0x4d5f, 0x4f4b, // M_OK
  0x535f, 0x4f4b, // S_OK
  0x4D41, 0x5346, // MASF - Brutal Unleashed
  0x5331, 0x4d31, // Darxide
  0x5332, 0x4d32,
  0x5333, 0x4d33,
  0x0000, 0x0000, // eq for doom
  0x0002, // Mortal Kombat
//  0, // pad
};

static u32 sh2_comm_faker(u32 a)
{
  static int f = 0;
  if (a == 0x28 && !p32x_csum_faked) {
    p32x_csum_faked = 1;
    return *(unsigned short *)(Pico.rom + 0x18e);
  }
  if (f >= sizeof(comm_fakevals) / sizeof(comm_fakevals[0]))
    f = 0;
  return comm_fakevals[f++];
}
#endif

// DMAC handling
static struct {
  unsigned int sar0, dar0, tcr0; // src addr, dst addr, transfer count
  unsigned int chcr0; // chan ctl
  unsigned int sar1, dar1, tcr1; // same for chan 1
  unsigned int chcr1;
  int pad[4];
  unsigned int dmaor;
} * dmac0;

static void dma_68k2sh2_do(void)
{
  unsigned short *dreqlen = &Pico32x.regs[0x10 / 2];
  int i;

  if (dmac0->tcr0 != *dreqlen)
    elprintf(EL_32X|EL_ANOMALY, "tcr0 and dreq len differ: %d != %d", dmac0->tcr0, *dreqlen);

  // HACK: assume bus is busy and SH2 is halted
  // XXX: use different mechanism for this, not poll det
  Pico32x.emu_flags |= P32XF_MSH2POLL; // id ? P32XF_SSH2POLL : P32XF_MSH2POLL;

  for (i = 0; i < Pico32x.dmac_ptr && dmac0->tcr0 > 0; i++) {
    elprintf(EL_32X, "dmaw [%08x] %04x, left %d", dmac0->dar0, Pico32x.dmac_fifo[i], *dreqlen);
    p32x_sh2_write16(dmac0->dar0, Pico32x.dmac_fifo[i], &msh2);
    dmac0->dar0 += 2;
    dmac0->tcr0--;
    (*dreqlen)--;
  }

  Pico32x.dmac_ptr = 0; // HACK
  Pico32x.regs[6 / 2] &= ~P32XS_FULL;
  if (*dreqlen == 0)
    Pico32x.regs[6 / 2] &= ~P32XS_68S; // transfer complete
  if (dmac0->tcr0 == 0) {
    dmac0->chcr0 |= 2; // DMA has ended normally
    p32x_poll_undetect(&sh2_poll[0], 0);
  }
}

// ------------------------------------------------------------------
// 68k regs

static u32 p32x_reg_read16(u32 a)
{
  a &= 0x3e;

#if 0
  if ((a & 0x30) == 0x20)
    return sh2_comm_faker(a);
#else
  if ((a & 0x30) == 0x20) {
    static u32 dr2 = 0;
    unsigned int cycles = SekCyclesDoneT();
    int comreg = 1 << (a & 0x0f) / 2;

    // evil X-Men proto polls in a dbra loop and expects it to expire..
    if (SekDar(2) != dr2)
      m68k_poll.cnt = 0;
    dr2 = SekDar(2);

    if (cycles - msh2.m68krcycles_done > 500)
      p32x_sync_sh2s(cycles);
    if (Pico32x.comm_dirty_sh2 & comreg)
      Pico32x.comm_dirty_sh2 &= ~comreg;
    else if (p32x_poll_detect(&m68k_poll, a, cycles, 0)) {
      SekSetStop(1);
      SekEndTimeslice(16);
    }
    dr2 = SekDar(2);
    goto out;
  }
#endif

  if (a == 2) { // INTM, INTS
    unsigned int cycles = SekCyclesDoneT();
    if (cycles - msh2.m68krcycles_done > 64)
      p32x_sync_sh2s(cycles);
    return ((Pico32x.sh2irqi[0] & P32XI_CMD) >> 4) | ((Pico32x.sh2irqi[1] & P32XI_CMD) >> 3);
  }

  if ((a & 0x30) == 0x30)
    return p32x_pwm_read16(a);

out:
  return Pico32x.regs[a / 2];
}

static void p32x_reg_write8(u32 a, u32 d)
{
  u16 *r = Pico32x.regs;
  a &= 0x3f;

  // for things like bset on comm port
  m68k_poll.cnt = 0;

  switch (a) {
    case 0: // adapter ctl
      r[0] = (r[0] & ~P32XS_FM) | ((d << 8) & P32XS_FM);
      return;
    case 1: // adapter ctl, RES bit writeable
      if ((d ^ r[0]) & d & P32XS_nRES)
        p32x_reset_sh2s();
      r[0] = (r[0] & ~P32XS_nRES) | (d & P32XS_nRES);
      return;
    case 3: // irq ctl
      if ((d & 1) && !(Pico32x.sh2irqi[0] & P32XI_CMD)) {
        p32x_sync_sh2s(SekCyclesDoneT());
        Pico32x.sh2irqi[0] |= P32XI_CMD;
        p32x_update_irls(0);
      }
      if ((d & 2) && !(Pico32x.sh2irqi[1] & P32XI_CMD)) {
        p32x_sync_sh2s(SekCyclesDoneT());
        Pico32x.sh2irqi[1] |= P32XI_CMD;
        p32x_update_irls(0);
      }
      return;
    case 5: // bank
      d &= 7;
      if (r[4 / 2] != d) {
        r[4 / 2] = d;
        bank_switch(d);
      }
      return;
    case 7: // DREQ ctl
      r[6 / 2] = (r[6 / 2] & P32XS_FULL) | (d & (P32XS_68S|P32XS_DMA|P32XS_RV));
      return;
    case 0x1b: // TV
      r[0x1a / 2] = d;
      return;
  }

  if ((a & 0x30) == 0x20) {
    u8 *r8 = (u8 *)r;
    int cycles = SekCyclesDoneT();
    int comreg;
    
    if (r8[a ^ 1] == d)
      return;
    
    comreg = 1 << (a & 0x0f) / 2;
    if (Pico32x.comm_dirty_68k & comreg)
      p32x_sync_sh2s(cycles);

    r8[a ^ 1] = d;
    p32x_poll_undetect(&sh2_poll[0], 0);
    p32x_poll_undetect(&sh2_poll[1], 0);
    Pico32x.comm_dirty_68k |= comreg;

    if (cycles - (int)msh2.m68krcycles_done > 120)
      p32x_sync_sh2s(cycles);
    return;
  }
}

static void p32x_reg_write16(u32 a, u32 d)
{
  u16 *r = Pico32x.regs;
  a &= 0x3e;

  // for things like bset on comm port
  m68k_poll.cnt = 0;

  switch (a) {
    case 0x00: // adapter ctl
      if ((d ^ r[0]) & d & P32XS_nRES)
        p32x_reset_sh2s();
      r[0] = (r[0] & ~(P32XS_FM|P32XS_nRES)) | (d & (P32XS_FM|P32XS_nRES));
      return;
    case 0x10: // DREQ len
      r[a / 2] = d & ~3;
      return;
    case 0x12: // FIFO reg
      if (!(r[6 / 2] & P32XS_68S)) {
        elprintf(EL_32X|EL_ANOMALY, "DREQ FIFO w16 without 68S?");
	return;
      }
      if (Pico32x.dmac_ptr < DMAC_FIFO_LEN) {
        Pico32x.dmac_fifo[Pico32x.dmac_ptr++] = d;
        if ((Pico32x.dmac_ptr & 3) == 0 && (dmac0->chcr0 & 3) == 1 && (dmac0->dmaor & 1))
          dma_68k2sh2_do();
        if (Pico32x.dmac_ptr == DMAC_FIFO_LEN)
          r[6 / 2] |= P32XS_FULL;
      }
      break;
  }

  // DREQ src, dst
  if      ((a & 0x38) == 0x08) {
    r[a / 2] = d;
    return;
  }
  // comm port
  else if ((a & 0x30) == 0x20) {
    int cycles = SekCyclesDoneT();
    int comreg;
    
    if (r[a / 2] == d)
      return;

    comreg = 1 << (a & 0x0f) / 2;
    if (Pico32x.comm_dirty_68k & comreg)
      p32x_sync_sh2s(cycles);

    r[a / 2] = d;
    p32x_poll_undetect(&sh2_poll[0], 0);
    p32x_poll_undetect(&sh2_poll[1], 0);
    Pico32x.comm_dirty_68k |= comreg;

    if (cycles - (int)msh2.m68krcycles_done > 120)
      p32x_sync_sh2s(cycles);
    return;
  }
  // PWM
  else if ((a & 0x30) == 0x30) {
    p32x_pwm_write16(a, d);
    return;
  }

  p32x_reg_write8(a + 1, d);
}

// ------------------------------------------------------------------
// VDP regs
static u32 p32x_vdp_read16(u32 a)
{
  a &= 0x0e;

  return Pico32x.vdp_regs[a / 2];
}

static void p32x_vdp_write8(u32 a, u32 d)
{
  u16 *r = Pico32x.vdp_regs;
  a &= 0x0f;

  // for FEN checks between writes
  sh2_poll[0].cnt = 0;

  // TODO: verify what's writeable
  switch (a) {
    case 0x01:
      // priority inversion is handled in palette
      if ((r[0] ^ d) & P32XV_PRI)
        Pico32x.dirty_pal = 1;
      r[0] = (r[0] & P32XV_nPAL) | (d & 0xff);
      break;
    case 0x03: // shift (for pp mode)
      r[2 / 2] = d & 1;
      break;
    case 0x05: // fill len
      r[4 / 2] = d & 0xff;
      break;
    case 0x0b:
      d &= 1;
      Pico32x.pending_fb = d;
      // if we are blanking and FS bit is changing
      if (((r[0x0a/2] & P32XV_VBLK) || (r[0] & P32XV_Mx) == 0) && ((r[0x0a/2] ^ d) & P32XV_FS)) {
        r[0x0a/2] ^= P32XV_FS;
	Pico32xSwapDRAM(d ^ 1);
        elprintf(EL_32X, "VDP FS: %d", r[0x0a/2] & P32XV_FS);
      }
      break;
  }
}

static void p32x_vdp_write16(u32 a, u32 d, u32 cycles)
{
  a &= 0x0e;
  if (a == 6) { // fill start
    Pico32x.vdp_regs[6 / 2] = d;
    return;
  }
  if (a == 8) { // fill data
    u16 *dram = Pico32xMem->dram[(Pico32x.vdp_regs[0x0a/2] & P32XV_FS) ^ 1];
    int len = Pico32x.vdp_regs[4 / 2] + 1;
    int len1 = len;
    a = Pico32x.vdp_regs[6 / 2];
    while (len1--) {
      dram[a] = d;
      a = (a & 0xff00) | ((a + 1) & 0xff);
    }
    Pico32x.vdp_regs[0x06 / 2] = a;
    Pico32x.vdp_regs[0x08 / 2] = d;
    if (cycles > 0) {
      Pico32x.vdp_regs[0x0a / 2] |= P32XV_nFEN;
      p32x_event_schedule(P32X_EVENT_FILLEND, cycles, len);
    }
    return;
  }

  p32x_vdp_write8(a | 1, d);
}

// ------------------------------------------------------------------
// SH2 regs

static u32 p32x_sh2reg_read16(u32 a, int cpuid)
{
  u16 *r = Pico32x.regs;
  a &= 0xfe; // ?

  switch (a) {
    case 0x00: // adapter/irq ctl
      return (r[0] & P32XS_FM) | Pico32x.sh2_regs[0] | Pico32x.sh2irq_mask[cpuid];
    case 0x04: // H count (often as comm too)
      if (p32x_poll_detect(&sh2_poll[cpuid], a, ash2_cycles_done(), 0))
        ash2_end_run(8);
      return Pico32x.sh2_regs[4 / 2];
    case 0x10: // DREQ len
      return r[a / 2];
  }

  // DREQ src, dst
  if ((a & 0x38) == 0x08)
    return r[a / 2];
  // comm port
  if ((a & 0x30) == 0x20) {
    int comreg = 1 << (a & 0x0f) / 2;
    if (Pico32x.comm_dirty_68k & comreg)
      Pico32x.comm_dirty_68k &= ~comreg;
    else if (p32x_poll_detect(&sh2_poll[cpuid], a, ash2_cycles_done(), 0))
      ash2_end_run(8);
    return r[a / 2];
  }
  if ((a & 0x30) == 0x30) {
    sh2_poll[cpuid].cnt = 0;
    return p32x_pwm_read16(a);
  }

  return 0;
}

static void p32x_sh2reg_write8(u32 a, u32 d, int cpuid)
{
  a &= 0xff;
  switch (a) {
    case 0: // FM
      Pico32x.regs[0] &= ~P32XS_FM;
      Pico32x.regs[0] |= (d << 8) & P32XS_FM;
      return;
    case 1: // 
      Pico32x.sh2irq_mask[cpuid] = d & 0x8f;
      Pico32x.sh2_regs[0] &= ~0x80;
      Pico32x.sh2_regs[0] |= d & 0x80;
      if (d & 1)
        p32x_pwm_schedule(sh2s[cpuid].m68krcycles_done); // XXX: timing?
      p32x_update_irls(1);
      return;
    case 5: // H count
      Pico32x.sh2_regs[4 / 2] = d & 0xff;
      p32x_poll_undetect(&sh2_poll[cpuid ^ 1], 0);
      return;
  }

  if ((a & 0x30) == 0x20) {
    u8 *r8 = (u8 *)Pico32x.regs;
    int comreg;
    if (r8[a ^ 1] == d)
      return;

    r8[a ^ 1] = d;
    if (p32x_poll_undetect(&m68k_poll, 0))
      SekSetStop(0);
    p32x_poll_undetect(&sh2_poll[cpuid ^ 1], 0);
    comreg = 1 << (a & 0x0f) / 2;
    Pico32x.comm_dirty_sh2 |= comreg;
    return;
  }
}

static void p32x_sh2reg_write16(u32 a, u32 d, int cpuid)
{
  a &= 0xfe;

  // comm
  if ((a & 0x30) == 0x20) {
    int comreg;
    if (Pico32x.regs[a / 2] == d)
      return;

    Pico32x.regs[a / 2] = d;
    if (p32x_poll_undetect(&m68k_poll, 0))
      SekSetStop(0);
    p32x_poll_undetect(&sh2_poll[cpuid ^ 1], 0);
    comreg = 1 << (a & 0x0f) / 2;
    Pico32x.comm_dirty_sh2 |= comreg;
    return;
  }
  // PWM
  else if ((a & 0x30) == 0x30) {
    p32x_pwm_write16(a, d);
    return;
  }

  switch (a) {
    case 0: // FM
      Pico32x.regs[0] &= ~P32XS_FM;
      Pico32x.regs[0] |= d & P32XS_FM;
      break;
    case 0x14: Pico32x.sh2irqs &= ~P32XI_VRES; goto irls;
    case 0x16: Pico32x.sh2irqs &= ~P32XI_VINT; goto irls;
    case 0x18: Pico32x.sh2irqs &= ~P32XI_HINT; goto irls;
    case 0x1a: Pico32x.sh2irqi[cpuid] &= ~P32XI_CMD; goto irls;
    case 0x1c:
      Pico32x.sh2irqs &= ~P32XI_PWM;
      if (!(Pico32x.emu_flags & P32XF_PWM_PEND))
        p32x_pwm_schedule(sh2s[cpuid].m68krcycles_done); // timing?
      goto irls;
  }

  p32x_sh2reg_write8(a | 1, d, cpuid);
  return;

irls:
  p32x_update_irls(1);
}

// ------------------------------------------------------------------
// SH2 internal peripherals
// we keep them in little endian format
static u32 sh2_peripheral_read8(u32 a, int id)
{
  u8 *r = (void *)Pico32xMem->sh2_peri_regs[id];
  u32 d;

  a &= 0x1ff;
  d = PREG8(r, a);

  elprintf(EL_32X, "%csh2 peri r8  [%08x]       %02x @%06x", id ? 's' : 'm', a | ~0x1ff, d, sh2_pc(id));
  return d;
}

static u32 sh2_peripheral_read16(u32 a, int id)
{
  u16 *r = (void *)Pico32xMem->sh2_peri_regs[id];
  u32 d;

  a &= 0x1ff;
  d = r[(a / 2) ^ 1];

  elprintf(EL_32X, "%csh2 peri r16 [%08x]     %04x @%06x", id ? 's' : 'm', a | ~0x1ff, d, sh2_pc(id));
  return d;
}

static u32 sh2_peripheral_read32(u32 a, int id)
{
  u32 d;
  a &= 0x1fc;
  d = Pico32xMem->sh2_peri_regs[id][a / 4];

  elprintf(EL_32X, "%csh2 peri r32 [%08x] %08x @%06x", id ? 's' : 'm', a | ~0x1ff, d, sh2_pc(id));
  return d;
}

static int REGPARM(3) sh2_peripheral_write8(u32 a, u32 d, int id)
{
  u8 *r = (void *)Pico32xMem->sh2_peri_regs[id];
  elprintf(EL_32X, "%csh2 peri w8  [%08x]       %02x @%06x", id ? 's' : 'm', a, d, sh2_pc(id));

  a &= 0x1ff;
  PREG8(r, a) = d;

  // X-men SCI hack
  if ((a == 2 &&  (d & 0x20)) || // transmiter enabled
      (a == 4 && !(d & 0x80))) { // valid data in TDR
    void *oregs = Pico32xMem->sh2_peri_regs[id ^ 1];
    if ((PREG8(oregs, 2) & 0x50) == 0x50) { // receiver + irq enabled
      int level = PREG8(oregs, 0x60) >> 4;
      int vector = PREG8(oregs, 0x63) & 0x7f;
      elprintf(EL_32X, "%csh2 SCI recv irq (%d, %d)", (id ^ 1) ? 's' : 'm', level, vector);
      sh2_internal_irq(&sh2s[id ^ 1], level, vector);
      return 1;
    }
  }
  return 0;
}

static int REGPARM(3) sh2_peripheral_write16(u32 a, u32 d, int id)
{
  u16 *r = (void *)Pico32xMem->sh2_peri_regs[id];
  elprintf(EL_32X, "%csh2 peri w16 [%08x]     %04x @%06x", id ? 's' : 'm', a, d, sh2_pc(id));

  a &= 0x1ff;

  // evil WDT
  if (a == 0x80) {
    if ((d & 0xff00) == 0xa500) { // WTCSR
      PREG8(r, 0x80) = d;
      p32x_timers_recalc();
    }
    if ((d & 0xff00) == 0x5a00) // WTCNT
      PREG8(r, 0x81) = d;
    return 0;
  }

  r[(a / 2) ^ 1] = d;
  return 0;
}

static void sh2_peripheral_write32(u32 a, u32 d, int id)
{
  u32 *r = Pico32xMem->sh2_peri_regs[id];
  elprintf(EL_32X, "%csh2 peri w32 [%08x] %08x @%06x", id ? 's' : 'm', a, d, sh2_pc(id));

  a &= 0x1fc;
  r[a / 4] = d;

  switch (a) {
    // division unit (TODO: verify):
    case 0x104: // DVDNT: divident L, starts divide
      elprintf(EL_32X, "%csh2 divide %08x / %08x", id ? 's' : 'm', d, r[0x100 / 4]);
      if (r[0x100 / 4]) {
        signed int divisor = r[0x100 / 4];
                       r[0x118 / 4] = r[0x110 / 4] = (signed int)d % divisor;
        r[0x104 / 4] = r[0x11c / 4] = r[0x114 / 4] = (signed int)d / divisor;
      }
      else
        r[0x110 / 4] = r[0x114 / 4] = r[0x118 / 4] = r[0x11c / 4] = 0; // ?
      break;
    case 0x114:
      elprintf(EL_32X, "%csh2 divide %08x%08x / %08x @%08x",
        id ? 's' : 'm', r[0x110 / 4], d, r[0x100 / 4], sh2_pc(id));
      if (r[0x100 / 4]) {
        signed long long divident = (signed long long)r[0x110 / 4] << 32 | d;
        signed int divisor = r[0x100 / 4];
        // XXX: undocumented mirroring to 0x118,0x11c?
        r[0x118 / 4] = r[0x110 / 4] = divident % divisor;
        divident /= divisor;
        r[0x11c / 4] = r[0x114 / 4] = divident;
        divident >>= 31;
        if ((unsigned long long)divident + 1 > 1) {
          //elprintf(EL_32X, "%csh2 divide overflow! @%08x", id ? 's' : 'm', sh2_pc(id));
          r[0x11c / 4] = r[0x114 / 4] = divident > 0 ? 0x7fffffff : 0x80000000; // overflow
        }
      }
      else
        r[0x110 / 4] = r[0x114 / 4] = r[0x118 / 4] = r[0x11c / 4] = 0; // ?
      break;
  }

  if ((a == 0x1b0 || a == 0x18c) && (dmac0->chcr0 & 3) == 1 && (dmac0->dmaor & 1)) {
    elprintf(EL_32X, "sh2 DMA %08x -> %08x, cnt %d, chcr %04x @%06x",
      dmac0->sar0, dmac0->dar0, dmac0->tcr0, dmac0->chcr0, sh2_pc(id));
    dmac0->tcr0 &= 0xffffff;

    // HACK: assume 68k starts writing soon and end the timeslice
    ash2_end_run(16);

    // DREQ is only sent after first 4 words are written.
    // we do multiple of 4 words to avoid messing up alignment
    if (dmac0->sar0 == 0x20004012 && Pico32x.dmac_ptr && (Pico32x.dmac_ptr & 3) == 0) {
      elprintf(EL_32X, "68k -> sh2 DMA");
      dma_68k2sh2_do();
    }
  }
}

// ------------------------------------------------------------------
// 32x handlers

// after ADEN
static u32 PicoRead8_32x_on(u32 a)
{
  u32 d = 0;
  if ((a & 0xffc0) == 0x5100) { // a15100
    d = p32x_reg_read16(a);
    goto out_16to8;
  }

  if ((a & 0xfc00) != 0x5000)
    return PicoRead8_io(a);

  if ((a & 0xfff0) == 0x5180) { // a15180
    d = p32x_vdp_read16(a);
    goto out_16to8;
  }

  if ((a & 0xfe00) == 0x5200) { // a15200
    d = Pico32xMem->pal[(a & 0x1ff) / 2];
    goto out_16to8;
  }

  if ((a & 0xfffc) == 0x30ec) { // a130ec
    d = str_mars[a & 3];
    goto out;
  }

  elprintf(EL_UIO, "m68k unmapped r8  [%06x] @%06x", a, SekPc);
  return d;

out_16to8:
  if (a & 1)
    d &= 0xff;
  else
    d >>= 8;

out:
  elprintf(EL_32X, "m68k 32x r8  [%06x]   %02x @%06x", a, d, SekPc);
  return d;
}

static u32 PicoRead16_32x_on(u32 a)
{
  u32 d = 0;
  if ((a & 0xffc0) == 0x5100) { // a15100
    d = p32x_reg_read16(a);
    goto out;
  }

  if ((a & 0xfc00) != 0x5000)
    return PicoRead16_io(a);

  if ((a & 0xfff0) == 0x5180) { // a15180
    d = p32x_vdp_read16(a);
    goto out;
  }

  if ((a & 0xfe00) == 0x5200) { // a15200
    d = Pico32xMem->pal[(a & 0x1ff) / 2];
    goto out;
  }

  if ((a & 0xfffc) == 0x30ec) { // a130ec
    d = !(a & 2) ? ('M'<<8)|'A' : ('R'<<8)|'S';
    goto out;
  }

  elprintf(EL_UIO, "m68k unmapped r16 [%06x] @%06x", a, SekPc);
  return d;

out:
  elprintf(EL_32X, "m68k 32x r16 [%06x] %04x @%06x", a, d, SekPc);
  return d;
}

static void PicoWrite8_32x_on(u32 a, u32 d)
{
  if ((a & 0xfc00) == 0x5000)
    elprintf(EL_32X, "m68k 32x w8  [%06x]   %02x @%06x", a, d & 0xff, SekPc);

  if ((a & 0xffc0) == 0x5100) { // a15100
    p32x_reg_write8(a, d);
    return;
  }

  if ((a & 0xfc00) != 0x5000) {
    PicoWrite8_io(a, d);
    return;
  }

  if ((a & 0xfff0) == 0x5180) { // a15180
    p32x_vdp_write8(a, d);
    return;
  }

  // TODO: verify
  if ((a & 0xfe00) == 0x5200) { // a15200
    elprintf(EL_32X|EL_ANOMALY, "m68k 32x PAL w8  [%06x]   %02x @%06x", a, d & 0xff, SekPc);
    ((u8 *)Pico32xMem->pal)[(a & 0x1ff) ^ 1] = d;
    Pico32x.dirty_pal = 1;
    return;
  }

  elprintf(EL_UIO, "m68k unmapped w8  [%06x]   %02x @%06x", a, d & 0xff, SekPc);
}

static void PicoWrite16_32x_on(u32 a, u32 d)
{
  if ((a & 0xfc00) == 0x5000)
    elprintf(EL_32X, "m68k 32x w16 [%06x] %04x @%06x", a, d & 0xffff, SekPc);

  if ((a & 0xffc0) == 0x5100) { // a15100
    p32x_reg_write16(a, d);
    return;
  }

  if ((a & 0xfc00) != 0x5000) {
    PicoWrite16_io(a, d);
    return;
  }

  if ((a & 0xfff0) == 0x5180) { // a15180
    p32x_vdp_write16(a, d, 0); // FIXME?
    return;
  }

  if ((a & 0xfe00) == 0x5200) { // a15200
    Pico32xMem->pal[(a & 0x1ff) / 2] = d;
    Pico32x.dirty_pal = 1;
    return;
  }

  elprintf(EL_UIO, "m68k unmapped w16 [%06x] %04x @%06x", a, d & 0xffff, SekPc);
}

// before ADEN
u32 PicoRead8_32x(u32 a)
{
  u32 d = 0;
  if ((a & 0xffc0) == 0x5100) { // a15100
    // regs are always readable
    d = ((u8 *)Pico32x.regs)[(a & 0x3f) ^ 1];
    goto out;
  }

  if ((a & 0xfffc) == 0x30ec) { // a130ec
    d = str_mars[a & 3];
    goto out;
  }

  elprintf(EL_UIO, "m68k unmapped r8  [%06x] @%06x", a, SekPc);
  return d;

out:
  elprintf(EL_32X, "m68k 32x r8  [%06x]   %02x @%06x", a, d, SekPc);
  return d;
}

u32 PicoRead16_32x(u32 a)
{
  u32 d = 0;
  if ((a & 0xffc0) == 0x5100) { // a15100
    d = Pico32x.regs[(a & 0x3f) / 2];
    goto out;
  }

  if ((a & 0xfffc) == 0x30ec) { // a130ec
    d = !(a & 2) ? ('M'<<8)|'A' : ('R'<<8)|'S';
    goto out;
  }

  elprintf(EL_UIO, "m68k unmapped r16 [%06x] @%06x", a, SekPc);
  return d;

out:
  elprintf(EL_32X, "m68k 32x r16 [%06x] %04x @%06x", a, d, SekPc);
  return d;
}

void PicoWrite8_32x(u32 a, u32 d)
{
  if ((a & 0xffc0) == 0x5100) { // a15100
    u16 *r = Pico32x.regs;

    elprintf(EL_32X, "m68k 32x w8  [%06x]   %02x @%06x", a, d & 0xff, SekPc);
    a &= 0x3f;
    if (a == 1) {
      if ((d ^ r[0]) & d & P32XS_ADEN) {
        Pico32xStartup();
        r[0] &= ~P32XS_nRES; // causes reset if specified by this write
        r[0] |= P32XS_ADEN;
        p32x_reg_write8(a, d); // forward for reset processing
      }
      return;
    }

    // allow only COMM for now
    if ((a & 0x30) == 0x20) {
      u8 *r8 = (u8 *)r;
      r8[a ^ 1] = d;
    }
    return;
  }

  elprintf(EL_UIO, "m68k unmapped w8  [%06x]   %02x @%06x", a, d & 0xff, SekPc);
}

void PicoWrite16_32x(u32 a, u32 d)
{
  if ((a & 0xffc0) == 0x5100) { // a15100
    u16 *r = Pico32x.regs;

    elprintf(EL_UIO, "m68k 32x w16 [%06x] %04x @%06x", a, d & 0xffff, SekPc);
    a &= 0x3e;
    if (a == 0) {
      if ((d ^ r[0]) & d & P32XS_ADEN) {
        Pico32xStartup();
        r[0] &= ~P32XS_nRES; // causes reset if specified by this write
        r[0] |= P32XS_ADEN;
        p32x_reg_write16(a, d); // forward for reset processing
      }
      return;
    }

    // allow only COMM for now
    if ((a & 0x30) == 0x20)
      r[a / 2] = d;
    return;
  }

  elprintf(EL_UIO, "m68k unmapped w16 [%06x] %04x @%06x", a, d & 0xffff, SekPc);
}

// -----------------------------------------------------------------

// hint vector is writeable
static void PicoWrite8_hint(u32 a, u32 d)
{
  if ((a & 0xfffc) == 0x0070) {
    Pico32xMem->m68k_rom[a ^ 1] = d;
    return;
  }

  elprintf(EL_UIO, "m68k unmapped w8  [%06x]   %02x @%06x", a, d & 0xff, SekPc);
}

static void PicoWrite16_hint(u32 a, u32 d)
{
  if ((a & 0xfffc) == 0x0070) {
    ((u16 *)Pico32xMem->m68k_rom)[a/2] = d;
    return;
  }

  elprintf(EL_UIO, "m68k unmapped w16 [%06x] %04x @%06x", a, d & 0xffff, SekPc);
}

static void bank_switch(int b)
{
  unsigned int rs, bank;

  bank = b << 20;
  if (bank >= Pico.romsize) {
    elprintf(EL_32X|EL_ANOMALY, "missing bank @ %06x", bank);
    return;
  }

  // 32X ROM (unbanked, XXX: consider mirroring?)
  rs = (Pico.romsize + M68K_BANK_MASK) & ~M68K_BANK_MASK;
  rs -= bank;
  if (rs > 0x100000)
    rs = 0x100000;
  cpu68k_map_set(m68k_read8_map,   0x900000, 0x900000 + rs - 1, Pico.rom + bank, 0);
  cpu68k_map_set(m68k_read16_map,  0x900000, 0x900000 + rs - 1, Pico.rom + bank, 0);

  elprintf(EL_32X, "bank %06x-%06x -> %06x", 0x900000, 0x900000 + rs - 1, bank);

#ifdef EMU_F68K
  // setup FAME fetchmap
  for (rs = 0x90; rs < 0xa0; rs++)
    PicoCpuFM68k.Fetch[rs] = (unsigned long)Pico.rom + bank - 0x900000;
#endif
}

// -----------------------------------------------------------------
//                              SH2  
// -----------------------------------------------------------------

// read8
static u32 sh2_read8_unmapped(u32 a, int id)
{
  elprintf(EL_UIO, "%csh2 unmapped r8  [%08x]       %02x @%06x",
    id ? 's' : 'm', a, 0, sh2_pc(id));
  return 0;
}

static u32 sh2_read8_cs0(u32 a, int id)
{
  u32 d = 0;

  // 0x3ff00 is veridied
  if ((a & 0x3ff00) == 0x4000) {
    d = p32x_sh2reg_read16(a, id);
    goto out_16to8;
  }

  if ((a & 0x3ff00) == 0x4100) {
    d = p32x_vdp_read16(a);
    if (p32x_poll_detect(&sh2_poll[id], a, ash2_cycles_done(), 1))
      ash2_end_run(8);
    goto out_16to8;
  }

  // TODO: mirroring?
  if (id == 0 && a < sizeof(Pico32xMem->sh2_rom_m))
    return Pico32xMem->sh2_rom_m[a ^ 1];
  if (id == 1 && a < sizeof(Pico32xMem->sh2_rom_s))
    return Pico32xMem->sh2_rom_s[a ^ 1];

  if ((a & 0x3fe00) == 0x4200) {
    d = Pico32xMem->pal[(a & 0x1ff) / 2];
    goto out_16to8;
  }

  return sh2_read8_unmapped(a, id);

out_16to8:
  if (a & 1)
    d &= 0xff;
  else
    d >>= 8;

  elprintf(EL_32X, "%csh2 r8  [%08x]       %02x @%06x",
    id ? 's' : 'm', a, d, sh2_pc(id));
  return d;
}

static u32 sh2_read8_da(u32 a, int id)
{
  return Pico32xMem->data_array[id][(a & 0xfff) ^ 1];
}

// read16
static u32 sh2_read16_unmapped(u32 a, int id)
{
  elprintf(EL_UIO, "%csh2 unmapped r16 [%08x]     %04x @%06x",
    id ? 's' : 'm', a, 0, sh2_pc(id));
  return 0;
}

static u32 sh2_read16_cs0(u32 a, int id)
{
  u32 d = 0;

  if ((a & 0x3ff00) == 0x4000) {
    d = p32x_sh2reg_read16(a, id);
    if (!(EL_LOGMASK & EL_PWM) && (a & 0x30) == 0x30) // hide PWM
      return d;
    goto out;
  }

  if ((a & 0x3ff00) == 0x4100) {
    d = p32x_vdp_read16(a);
    if (p32x_poll_detect(&sh2_poll[id], a, ash2_cycles_done(), 1))
      ash2_end_run(8);
    goto out;
  }

  if (id == 0 && a < sizeof(Pico32xMem->sh2_rom_m))
    return *(u16 *)(Pico32xMem->sh2_rom_m + a);
  if (id == 1 && a < sizeof(Pico32xMem->sh2_rom_s))
    return *(u16 *)(Pico32xMem->sh2_rom_s + a);

  if ((a & 0x3fe00) == 0x4200) {
    d = Pico32xMem->pal[(a & 0x1ff) / 2];
    goto out;
  }

  return sh2_read16_unmapped(a, id);

out:
  elprintf(EL_32X, "%csh2 r16 [%08x]     %04x @%06x",
    id ? 's' : 'm', a, d, sh2_pc(id));
  return d;
}

static u32 sh2_read16_da(u32 a, int id)
{
  return ((u16 *)Pico32xMem->data_array[id])[(a & 0xfff) / 2];
}

static int REGPARM(3) sh2_write_ignore(u32 a, u32 d, int id)
{
  return 0;
}

// write8
static int REGPARM(3) sh2_write8_unmapped(u32 a, u32 d, int id)
{
  elprintf(EL_UIO, "%csh2 unmapped w8  [%08x]       %02x @%06x",
    id ? 's' : 'm', a, d & 0xff, sh2_pc(id));
  return 0;
}

static int REGPARM(3) sh2_write8_cs0(u32 a, u32 d, int id)
{
  elprintf(EL_32X, "%csh2 w8  [%08x]       %02x @%06x",
    id ? 's' : 'm', a, d & 0xff, sh2_pc(id));

  if ((a & 0x3ff00) == 0x4100) {
    p32x_vdp_write8(a, d);
    return 0;
  }

  if ((a & 0x3ff00) == 0x4000) {
    p32x_sh2reg_write8(a, d, id);
    return 1;
  }

  return sh2_write8_unmapped(a, d, id);
}

/* quirk: in both normal and overwrite areas only nonzero values go through */
#define sh2_write8_dramN(n) \
  if ((d & 0xff) != 0) { \
    u8 *dram = (u8 *)Pico32xMem->dram[n]; \
    dram[(a & 0x1ffff) ^ 1] = d; \
  } \
  return 0;

static int REGPARM(3) sh2_write8_dram0(u32 a, u32 d, int id)
{
  sh2_write8_dramN(0);
}

static int REGPARM(3) sh2_write8_dram1(u32 a, u32 d, int id)
{
  sh2_write8_dramN(1);
}

static int REGPARM(3) sh2_write8_sdram(u32 a, u32 d, int id)
{
  u32 a1 = a & 0x3ffff;
#ifdef DRC_SH2
  int t = Pico32xMem->drcblk_ram[a1 >> SH2_DRCBLK_RAM_SHIFT];
  if (t)
    sh2_drc_wcheck_ram(a, t, id);
#endif
  Pico32xMem->sdram[a1 ^ 1] = d;
  return 0;
}

static int REGPARM(3) sh2_write8_da(u32 a, u32 d, int id)
{
  u32 a1 = a & 0xfff;
#ifdef DRC_SH2
  int t = Pico32xMem->drcblk_da[id][a1 >> SH2_DRCBLK_DA_SHIFT];
  if (t)
    sh2_drc_wcheck_da(a, t, id);
#endif
  Pico32xMem->data_array[id][a1 ^ 1] = d;
  return 0;
}

// write16
static int REGPARM(3) sh2_write16_unmapped(u32 a, u32 d, int id)
{
  elprintf(EL_UIO, "%csh2 unmapped w16 [%08x]     %04x @%06x",
    id ? 's' : 'm', a, d & 0xffff, sh2_pc(id));
  return 0;
}

static int REGPARM(3) sh2_write16_cs0(u32 a, u32 d, int id)
{
  if (((EL_LOGMASK & EL_PWM) || (a & 0x30) != 0x30)) // hide PWM
    elprintf(EL_32X, "%csh2 w16 [%08x]     %04x @%06x",
      id ? 's' : 'm', a, d & 0xffff, sh2_pc(id));

  if ((a & 0x3ff00) == 0x4100) {
    sh2_poll[id].cnt = 0; // for poll before VDP accesses
    p32x_vdp_write16(a, d, sh2s[id].m68krcycles_done);
    return 0;
  }

  if ((a & 0x3fe00) == 0x4200) {
    Pico32xMem->pal[(a & 0x1ff) / 2] = d;
    Pico32x.dirty_pal = 1;
    return 0;
  }

  if ((a & 0x3ff00) == 0x4000) {
    p32x_sh2reg_write16(a, d, id);
    return 1;
  }

  return sh2_write16_unmapped(a, d, id);
}

#define sh2_write16_dramN(n) \
  u16 *pd = &Pico32xMem->dram[n][(a & 0x1ffff) / 2]; \
  if (!(a & 0x20000)) { \
    *pd = d; \
    return 0; \
  } \
  /* overwrite */ \
  if (!(d & 0xff00)) d |= *pd & 0xff00; \
  if (!(d & 0x00ff)) d |= *pd & 0x00ff; \
  *pd = d; \
  return 0

static int REGPARM(3) sh2_write16_dram0(u32 a, u32 d, int id)
{
  sh2_write16_dramN(0);
}

static int REGPARM(3) sh2_write16_dram1(u32 a, u32 d, int id)
{
  sh2_write16_dramN(1);
}

static int REGPARM(3) sh2_write16_sdram(u32 a, u32 d, int id)
{
  u32 a1 = a & 0x3ffff;
#ifdef DRC_SH2
  int t = Pico32xMem->drcblk_ram[a1 >> SH2_DRCBLK_RAM_SHIFT];
  if (t)
    sh2_drc_wcheck_ram(a, t, id);
#endif
  ((u16 *)Pico32xMem->sdram)[a1 / 2] = d;
  return 0;
}

static int REGPARM(3) sh2_write16_da(u32 a, u32 d, int id)
{
  u32 a1 = a & 0xfff;
#ifdef DRC_SH2
  int t = Pico32xMem->drcblk_da[id][a1 >> SH2_DRCBLK_DA_SHIFT];
  if (t)
    sh2_drc_wcheck_da(a, t, id);
#endif
  ((u16 *)Pico32xMem->data_array[id])[a1 / 2] = d;
  return 0;
}


typedef struct {
  uptr addr; // stores (membase >> 1) or ((handler >> 1) | (1<<31))
  u32 mask;
} sh2_memmap;

typedef u32 (sh2_read_handler)(u32 a, int id);
typedef int REGPARM(3) (sh2_write_handler)(u32 a, u32 d, int id);

#define SH2MAP_ADDR2OFFS_R(a) \
  ((((a) >> 25) & 3) | (((a) >> 27) & 0x1c))

#define SH2MAP_ADDR2OFFS_W(a) \
  ((u32)(a) >> SH2_WRITE_SHIFT)

u32 REGPARM(2) p32x_sh2_read8(u32 a, SH2 *sh2)
{
  const sh2_memmap *sh2_map = sh2->read8_map;
  uptr p;

  sh2_map += SH2MAP_ADDR2OFFS_R(a);
  p = sh2_map->addr;
  if (map_flag_set(p))
    return ((sh2_read_handler *)(p << 1))(a, sh2->is_slave);
  else
    return *(u8 *)((p << 1) + ((a & sh2_map->mask) ^ 1));
}

u32 REGPARM(2) p32x_sh2_read16(u32 a, SH2 *sh2)
{
  const sh2_memmap *sh2_map = sh2->read16_map;
  uptr p;

  sh2_map += SH2MAP_ADDR2OFFS_R(a);
  p = sh2_map->addr;
  if (map_flag_set(p))
    return ((sh2_read_handler *)(p << 1))(a, sh2->is_slave);
  else
    return *(u16 *)((p << 1) + ((a & sh2_map->mask) & ~1));
}

u32 REGPARM(2) p32x_sh2_read32(u32 a, SH2 *sh2)
{
  const sh2_memmap *sh2_map = sh2->read16_map;
  sh2_read_handler *handler;
  u32 offs;
  uptr p;

  offs = SH2MAP_ADDR2OFFS_R(a);
  sh2_map += offs;
  p = sh2_map->addr;
  if (!map_flag_set(p)) {
    // XXX: maybe 32bit access instead with ror?
    u16 *pd = (u16 *)((p << 1) + ((a & sh2_map->mask) & ~1));
    return (pd[0] << 16) | pd[1];
  }

  if (offs == 0x1f)
    return sh2_peripheral_read32(a, sh2->is_slave);

  handler = (sh2_read_handler *)(p << 1);
  return (handler(a, sh2->is_slave) << 16) | handler(a + 2, sh2->is_slave);
}

// return nonzero if write potentially causes an interrupt (used by drc)
int REGPARM(3) p32x_sh2_write8(u32 a, u32 d, SH2 *sh2)
{
  const void **sh2_wmap = sh2->write8_tab;
  sh2_write_handler *wh;

  wh = sh2_wmap[SH2MAP_ADDR2OFFS_W(a)];
  return wh(a, d, sh2->is_slave);
}

int REGPARM(3) p32x_sh2_write16(u32 a, u32 d, SH2 *sh2)
{
  const void **sh2_wmap = sh2->write16_tab;
  sh2_write_handler *wh;

  wh = sh2_wmap[SH2MAP_ADDR2OFFS_W(a)];
  return wh(a, d, sh2->is_slave);
}

int REGPARM(3) p32x_sh2_write32(u32 a, u32 d, SH2 *sh2)
{
  const void **sh2_wmap = sh2->write16_tab;
  sh2_write_handler *handler;
  u32 offs;

  offs = SH2MAP_ADDR2OFFS_W(a);

  if (offs == SH2MAP_ADDR2OFFS_W(0xffffc000)) {
    sh2_peripheral_write32(a, d, sh2->is_slave);
    return 0;
  }

  handler = sh2_wmap[offs];
  handler(a, d >> 16, sh2->is_slave);
  handler(a + 2, d, sh2->is_slave);
  return 0;
}

// -----------------------------------------------------------------

static const u16 msh2_code[] = {
  // trap instructions
  0xaffe, // bra <self>
  0x0009, // nop
  // have to wait a bit until m68k initial program finishes clearing stuff
  // to avoid races with game SH2 code, like in Tempo
  0xd004, // mov.l   @(_m_ok,pc), r0
  0xd105, // mov.l   @(_cnt,pc), r1
  0xd205, // mov.l   @(_start,pc), r2
  0x71ff, // add     #-1, r1
  0x4115, // cmp/pl  r1
  0x89fc, // bt      -2
  0xc208, // mov.l   r0, @(h'20,gbr)
  0x6822, // mov.l   @r2, r8
  0x482b, // jmp     @r8
  0x0009, // nop
  ('M'<<8)|'_', ('O'<<8)|'K',
  0x0001, 0x0000,
  0x2200, 0x03e0  // master start pointer in ROM
};

static const u16 ssh2_code[] = {
  0xaffe, // bra <self>
  0x0009, // nop
  // code to wait for master, in case authentic master BIOS is used
  0xd104, // mov.l   @(_m_ok,pc), r1
  0xd206, // mov.l   @(_start,pc), r2
  0xc608, // mov.l   @(h'20,gbr), r0
  0x3100, // cmp/eq  r0, r1
  0x8bfc, // bf      #-2
  0xd003, // mov.l   @(_s_ok,pc), r0
  0xc209, // mov.l   r0, @(h'24,gbr)
  0x6822, // mov.l   @r2, r8
  0x482b, // jmp     @r8
  0x0009, // nop
  ('M'<<8)|'_', ('O'<<8)|'K',
  ('S'<<8)|'_', ('O'<<8)|'K',
  0x2200, 0x03e4  // slave start pointer in ROM
};

#define HWSWAP(x) (((x) << 16) | ((x) >> 16))
static void get_bios(void)
{
  u16 *ps;
  u32 *pl;
  int i;

  // M68K ROM
  if (p32x_bios_g != NULL) {
    elprintf(EL_STATUS|EL_32X, "32x: using supplied 68k BIOS");
    Byteswap(Pico32xMem->m68k_rom, p32x_bios_g, sizeof(Pico32xMem->m68k_rom));
  }
  else {
    // generate 68k ROM
    ps = (u16 *)Pico32xMem->m68k_rom;
    pl = (u32 *)ps;
    for (i = 1; i < 0xc0/4; i++)
      pl[i] = HWSWAP(0x880200 + (i - 1) * 6);

    // fill with nops
    for (i = 0xc0/2; i < 0x100/2; i++)
      ps[i] = 0x4e71;

#if 0
    ps[0xc0/2] = 0x46fc;
    ps[0xc2/2] = 0x2700; // move #0x2700,sr
    ps[0xfe/2] = 0x60fe; // jump to self
#else
    ps[0xfe/2] = 0x4e75; // rts
#endif
  }
  // fill remaining m68k_rom page with game ROM
  memcpy(Pico32xMem->m68k_rom_bank + sizeof(Pico32xMem->m68k_rom),
    Pico.rom + sizeof(Pico32xMem->m68k_rom),
    sizeof(Pico32xMem->m68k_rom_bank) - sizeof(Pico32xMem->m68k_rom));

  // MSH2
  if (p32x_bios_m != NULL) {
    elprintf(EL_STATUS|EL_32X, "32x: using supplied master SH2 BIOS");
    Byteswap(Pico32xMem->sh2_rom_m, p32x_bios_m, sizeof(Pico32xMem->sh2_rom_m));
  }
  else {
    pl = (u32 *)Pico32xMem->sh2_rom_m;

    // fill exception vector table to our trap address
    for (i = 0; i < 128; i++)
      pl[i] = HWSWAP(0x200);

    // startup code
    memcpy(Pico32xMem->sh2_rom_m + 0x200, msh2_code, sizeof(msh2_code));

    // reset SP
    pl[1] = pl[3] = HWSWAP(0x6040000);
    // start
    pl[0] = pl[2] = HWSWAP(0x204);
  }

  // SSH2
  if (p32x_bios_s != NULL) {
    elprintf(EL_STATUS|EL_32X, "32x: using supplied slave SH2 BIOS");
    Byteswap(Pico32xMem->sh2_rom_s, p32x_bios_s, sizeof(Pico32xMem->sh2_rom_s));
  }
  else {
    pl = (u32 *)Pico32xMem->sh2_rom_s;

    // fill exception vector table to our trap address
    for (i = 0; i < 128; i++)
      pl[i] = HWSWAP(0x200);

    // startup code
    memcpy(Pico32xMem->sh2_rom_s + 0x200, ssh2_code, sizeof(ssh2_code));

    // reset SP
    pl[1] = pl[3] = HWSWAP(0x603f800);
    // start
    pl[0] = pl[2] = HWSWAP(0x204);
  }
}

#define MAP_MEMORY(m) ((uptr)(m) >> 1)
#define MAP_HANDLER(h) ( ((uptr)(h) >> 1) | ((uptr)1 << (sizeof(uptr) * 8 - 1)) )

static sh2_memmap sh2_read8_map[0x20], sh2_read16_map[0x20];
// for writes we are using handlers only
static sh2_write_handler *sh2_write8_map[0x80], *sh2_write16_map[0x80];

void Pico32xSwapDRAM(int b)
{
  cpu68k_map_set(m68k_read8_map,   0x840000, 0x85ffff, Pico32xMem->dram[b], 0);
  cpu68k_map_set(m68k_read16_map,  0x840000, 0x85ffff, Pico32xMem->dram[b], 0);
  cpu68k_map_set(m68k_write8_map,  0x840000, 0x85ffff, Pico32xMem->dram[b], 0);
  cpu68k_map_set(m68k_write16_map, 0x840000, 0x85ffff, Pico32xMem->dram[b], 0);

  // SH2
  sh2_read8_map[2].addr   = sh2_read8_map[6].addr   =
  sh2_read16_map[2].addr  = sh2_read16_map[6].addr  = MAP_MEMORY(Pico32xMem->dram[b]);

  sh2_write8_map[0x04/2]  = sh2_write8_map[0x24/2]  = b ? sh2_write8_dram1 : sh2_write8_dram0;
  sh2_write16_map[0x04/2] = sh2_write16_map[0x24/2] = b ? sh2_write16_dram1 : sh2_write16_dram0;
}

void PicoMemSetup32x(void)
{
  unsigned int rs;
  int i;

  Pico32xMem = plat_mmap(0x06000000, sizeof(*Pico32xMem), 0, 0);
  if (Pico32xMem == NULL) {
    elprintf(EL_STATUS, "OOM");
    return;
  }

  dmac0 = (void *)&Pico32xMem->sh2_peri_regs[0][0x180 / 4];

  get_bios();

  // cartridge area becomes unmapped
  // XXX: we take the easy way and don't unmap ROM,
  // so that we can avoid handling the RV bit.
  // m68k_map_unmap(0x000000, 0x3fffff);

  // MD ROM area
  rs = sizeof(Pico32xMem->m68k_rom_bank);
  cpu68k_map_set(m68k_read8_map,   0x000000, rs - 1, Pico32xMem->m68k_rom_bank, 0);
  cpu68k_map_set(m68k_read16_map,  0x000000, rs - 1, Pico32xMem->m68k_rom_bank, 0);
  cpu68k_map_set(m68k_write8_map,  0x000000, rs - 1, PicoWrite8_hint, 1); // TODO verify
  cpu68k_map_set(m68k_write16_map, 0x000000, rs - 1, PicoWrite16_hint, 1);

  // 32X ROM (unbanked, XXX: consider mirroring?)
  rs = (Pico.romsize + M68K_BANK_MASK) & ~M68K_BANK_MASK;
  if (rs > 0x80000)
    rs = 0x80000;
  cpu68k_map_set(m68k_read8_map,   0x880000, 0x880000 + rs - 1, Pico.rom, 0);
  cpu68k_map_set(m68k_read16_map,  0x880000, 0x880000 + rs - 1, Pico.rom, 0);
#ifdef EMU_F68K
  // setup FAME fetchmap
  PicoCpuFM68k.Fetch[0] = (unsigned long)Pico32xMem->m68k_rom;
  for (rs = 0x88; rs < 0x90; rs++)
    PicoCpuFM68k.Fetch[rs] = (unsigned long)Pico.rom - 0x880000;
#endif

  // 32X ROM (banked)
  bank_switch(0);

  // SYS regs
  cpu68k_map_set(m68k_read8_map,   0xa10000, 0xa1ffff, PicoRead8_32x_on, 1);
  cpu68k_map_set(m68k_read16_map,  0xa10000, 0xa1ffff, PicoRead16_32x_on, 1);
  cpu68k_map_set(m68k_write8_map,  0xa10000, 0xa1ffff, PicoWrite8_32x_on, 1);
  cpu68k_map_set(m68k_write16_map, 0xa10000, 0xa1ffff, PicoWrite16_32x_on, 1);

  // SH2 maps: A31,A30,A29,CS1,CS0
  // all unmapped by default
  for (i = 0; i < ARRAY_SIZE(sh2_read8_map); i++) {
    sh2_read8_map[i].addr   = MAP_HANDLER(sh2_read8_unmapped);
    sh2_read16_map[i].addr  = MAP_HANDLER(sh2_read16_unmapped);
  }

  for (i = 0; i < ARRAY_SIZE(sh2_write8_map); i++) {
    sh2_write8_map[i]       = sh2_write8_unmapped;
    sh2_write16_map[i]      = sh2_write16_unmapped;
  }

  // "purge area"
  for (i = 0x40; i <= 0x5f; i++) {
    sh2_write8_map[i >> 1]  =
    sh2_write16_map[i >> 1] = sh2_write_ignore;
  }

  // CS0
  sh2_read8_map[0].addr   = sh2_read8_map[4].addr   = MAP_HANDLER(sh2_read8_cs0);
  sh2_read16_map[0].addr  = sh2_read16_map[4].addr  = MAP_HANDLER(sh2_read16_cs0);
  sh2_write8_map[0x00/2]  = sh2_write8_map[0x20/2]  = sh2_write8_cs0;
  sh2_write16_map[0x00/2] = sh2_write16_map[0x20/2] = sh2_write16_cs0;
  // CS1 - ROM
  sh2_read8_map[1].addr   = sh2_read8_map[5].addr   =
  sh2_read16_map[1].addr  = sh2_read16_map[5].addr  = MAP_MEMORY(Pico.rom);
  sh2_read8_map[1].mask   = sh2_read8_map[5].mask   =
  sh2_read16_map[1].mask  = sh2_read16_map[5].mask  = 0x3fffff; // FIXME
  // CS2 - DRAM - done by Pico32xSwapDRAM()
  sh2_read8_map[2].mask   = sh2_read8_map[6].mask   =
  sh2_read16_map[2].mask  = sh2_read16_map[6].mask  = 0x01ffff;
  // CS3 - SDRAM
  sh2_read8_map[3].addr   = sh2_read8_map[7].addr   =
  sh2_read16_map[3].addr  = sh2_read16_map[7].addr  = MAP_MEMORY(Pico32xMem->sdram);
  sh2_write8_map[0x06/2]  = sh2_write8_map[0x26/2]  = sh2_write8_sdram;
  sh2_write16_map[0x06/2] = sh2_write16_map[0x26/2] = sh2_write16_sdram;
  sh2_read8_map[3].mask   = sh2_read8_map[7].mask   =
  sh2_read16_map[3].mask  = sh2_read16_map[7].mask  = 0x03ffff;
  // SH2 data array
  sh2_read8_map[0x18].addr   = MAP_HANDLER(sh2_read8_da);
  sh2_read16_map[0x18].addr  = MAP_HANDLER(sh2_read16_da);
  sh2_write8_map[0xc0/2]     = sh2_write8_da;
  sh2_write16_map[0xc0/2]    = sh2_write16_da;
  // SH2 IO
  sh2_read8_map[0x1f].addr   = MAP_HANDLER(sh2_peripheral_read8);
  sh2_read16_map[0x1f].addr  = MAP_HANDLER(sh2_peripheral_read16);
  sh2_write8_map[0xff/2]     = sh2_peripheral_write8;
  sh2_write16_map[0xff/2]    = sh2_peripheral_write16;

  // map DRAM area, both 68k and SH2
  Pico32xSwapDRAM(1);

  msh2.read8_map   = ssh2.read8_map   = sh2_read8_map;
  msh2.read16_map  = ssh2.read16_map  = sh2_read16_map;
  msh2.write8_tab  = ssh2.write8_tab  = (const void **)(void *)sh2_write8_map;
  msh2.write16_tab = ssh2.write16_tab = (const void **)(void *)sh2_write16_map;

  // setup poll detector
  m68k_poll.flag = P32XF_68KPOLL;
  m68k_poll.cyc_max = 64;
  sh2_poll[0].flag = P32XF_MSH2POLL;
  sh2_poll[0].cyc_max = 21;
  sh2_poll[1].flag = P32XF_SSH2POLL;
  sh2_poll[1].cyc_max = 16;

#ifdef DRC_SH2
  sh2_drc_mem_setup(&msh2);
  sh2_drc_mem_setup(&ssh2);
#endif
}

void Pico32xStateLoaded(void)
{
  bank_switch(Pico32x.regs[4 / 2]);
  Pico32xSwapDRAM((Pico32x.vdp_regs[0x0a / 2] & P32XV_FS) ^ P32XV_FS);
  p32x_poll_event(3, 0);
  Pico32x.dirty_pal = 1;
  memset(Pico32xMem->pwm, 0, sizeof(Pico32xMem->pwm));
  p32x_timers_recalc();
#ifdef DRC_SH2
  sh2_drc_flush_all();
#endif
}

// vim:shiftwidth=2:ts=2:expandtab
