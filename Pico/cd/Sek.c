// This is part of Pico Library

// (c) Copyright 2004 Dave, All rights reserved.
// (c) Copyright 2006 notaz, All rights reserved.
// Free for non-commercial use.

// For commercial use, separate licencing terms must be obtained.


#include "../PicoInt.h"


int SekCycleCntS68k=0; // cycles done in this frame
int SekCycleAimS68k=0; // cycle aim

#ifdef EMU_C68K
// ---------------------- Cyclone 68000 ----------------------
struct Cyclone PicoCpuS68k;
#endif

#ifdef EMU_M68K
// ---------------------- MUSASHI 68000 ----------------------
m68ki_cpu_core PicoS68kCPU; // Mega CD's CPU
#endif

static int irqs = 0; // TODO: 2 context


#ifdef EMU_M68K
static int SekIntAckS68k(int level)
{
  int level_new = 0;
  irqs &= ~(1 << level);
  irqs &= Pico_mcd->s68k_regs[0x33];
  if (irqs) {
    level_new = 6;
    while (level_new > 0) { if (irqs & (1 << level_new)) break; level_new--; }
  }

  dprintf("s68kACK %i -> %i", level, level_new);
  CPU_INT_LEVEL = level_new << 8;
  return M68K_INT_ACK_AUTOVECTOR;
}
#endif

#ifdef EMU_C68K
// interrupt acknowledgment
static void SekIntAck(int level)
{
  int level_new = 0;
  irqs &= ~(1 << level);
  irqs &= Pico_mcd->s68k_regs[0x33];
  if (irqs) {
    level_new = 6;
    while (level_new > 0) { if (irqs & (1 << level_new)) break; level_new--; }
  }

  dprintf("s68kACK %i -> %i", level, level_new);
  PicoCpuS68k.irq = level_new;
}

static void SekResetAck()
{
  dprintf("s68k: Reset encountered @ %06x", SekPcS68k);
}

static int SekUnrecognizedOpcode()
{
  unsigned int pc, op;
  pc = SekPcS68k;
  op = PicoCpuS68k.read16(pc);
  dprintf("Unrecognized Opcode %04x @ %06x", op, pc);
  //exit(1);
  return 0;
}
#endif



int SekInitS68k()
{
#ifdef EMU_C68K
//  CycloneInit();
  memset(&PicoCpuS68k,0,sizeof(PicoCpuS68k));
  PicoCpuS68k.IrqCallback=SekIntAck;
  PicoCpuS68k.ResetCallback=SekResetAck;
  PicoCpuS68k.UnrecognizedCallback=SekUnrecognizedOpcode;
#endif
#ifdef EMU_M68K
  {
    // Musashi is not very context friendly..
    void *oldcontext = m68ki_cpu_p;
    m68k_set_context(&PicoS68kCPU);
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_init();
    m68k_set_int_ack_callback(SekIntAckS68k);
//  m68k_pulse_reset(); // not yet, memmap is not set up
    m68k_set_context(oldcontext);
  }
#endif

  return 0;
}

// Reset the 68000:
int SekResetS68k()
{
  if (Pico.rom==NULL) return 1;

#ifdef EMU_C68K
  PicoCpuS68k.stopped=0;
  PicoCpuS68k.osp=0;
  PicoCpuS68k.srh =0x27; // Supervisor mode
  PicoCpuS68k.flags=4;   // Z set
  PicoCpuS68k.irq=0;
  PicoCpuS68k.a[7]=PicoCpuS68k.read32(0); // Stack Pointer
  PicoCpuS68k.membase=0;
  PicoCpuS68k.pc=PicoCpuS68k.checkpc(PicoCpuS68k.read32(4)); // Program Counter
#endif
#ifdef EMU_M68K
  {
    void *oldcontext = m68ki_cpu_p;

    m68k_set_context(&PicoS68kCPU);
    m68k_pulse_reset();
    m68k_set_context(oldcontext);
  }
#endif

  return 0;
}

int SekInterruptS68k(int irq)
{
  irqs |= 1 << irq;
#ifdef EMU_C68K
  PicoCpuS68k.irq=irq;
#endif
#ifdef EMU_M68K
  void *oldcontext = m68ki_cpu_p;
  m68k_set_context(&PicoS68kCPU);
  m68k_set_irq(irq); // raise irq (gets lowered after taken or must be done in ack)
  m68k_set_context(oldcontext);
#endif
  return 0;
}
