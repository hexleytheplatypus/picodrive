// This is part of Pico Library

// (c) Copyright 2004 Dave, All rights reserved.
// (c) Copyright 2007 notaz, All rights reserved.
// Free for non-commercial use.

// For commercial use, separate licencing terms must be obtained.


#include "../PicoInt.h"
#include "../sound/sound.h"


static int counter75hz = 0; // TODO: move 2 context


int PicoInitMCD(void)
{
  SekInitS68k();
  Init_CD_Driver();

  return 0;
}


void PicoExitMCD(void)
{
  End_CD_Driver();
}

int PicoResetMCD(int hard)
{
  // clear everything except BIOS
  memset(Pico_mcd->prg_ram, 0, sizeof(mcd_state) - sizeof(Pico_mcd->bios));
  *(unsigned int *)(Pico_mcd->bios + 0x70) = 0xffffffff; // reset hint vector (simplest way to implement reg6)
  PicoMCD |= 2; // s68k reset pending. TODO: move
  Pico_mcd->s68k_regs[3] = 1; // 2M word RAM mode with m68k access after reset
  counter75hz = 0;

  LC89510_Reset();
  Reset_CD();

  return 0;
}

static __inline void SekRun(int cyc)
{
  int cyc_do;
  SekCycleAim+=cyc;
  if((cyc_do=SekCycleAim-SekCycleCnt) < 0) return;
#if defined(EMU_C68K)
  PicoCpu.cycles=cyc_do;
  CycloneRun(&PicoCpu);
  SekCycleCnt+=cyc_do-PicoCpu.cycles;
#elif defined(EMU_M68K)
  m68k_set_context(&PicoM68kCPU);
  SekCycleCnt+=m68k_execute(cyc_do);
#endif
}

static __inline void SekRunS68k(int cyc)
{
  int cyc_do;
  SekCycleAimS68k+=cyc;
  if((cyc_do=SekCycleAimS68k-SekCycleCntS68k) < 0) return;
#if defined(EMU_C68K)
  PicoCpuS68k.cycles=cyc_do;
  CycloneRun(&PicoCpuS68k);
  SekCycleCntS68k+=cyc_do-PicoCpuS68k.cycles;
#elif defined(EMU_M68K)
  m68k_set_context(&PicoS68kCPU);
  SekCycleCntS68k+=m68k_execute(cyc_do);
#endif
}

static __inline void check_cd_dma(void)
{
	int ddx;

	if (!(Pico_mcd->scd.Status_CDC & 0x08)) return;

	ddx = Pico_mcd->s68k_regs[4] & 7;
	if (ddx <  2) return; // invalid
	if (ddx <  4) {
		Pico_mcd->s68k_regs[4] |= 0x40; // Data set ready in host port
		return;
	}
	if (ddx == 6) return; // invalid

	Update_CDC_TRansfer(ddx); // now go and do the actual transfer
}

// to be called on 224 or line_sample scanlines only
static __inline void getSamples(int y)
{
  if(y == 224) {
    //dprintf("sta%i: %i [%i]", (emustatus & 2), emustatus, y);
    if(emustatus & 2)
        sound_render(PsndLen/2, PsndLen-PsndLen/2);
    else sound_render(0, PsndLen);
    if (emustatus&1) emustatus|=2; else emustatus&=~2;
    if (PicoWriteSound) PicoWriteSound();
    // clear sound buffer
    memset(PsndOut, 0, (PicoOpt & 8) ? (PsndLen<<2) : (PsndLen<<1));
  }
  else if(emustatus & 3) {
    emustatus|= 2;
    emustatus&=~1;
    sound_render(0, PsndLen/2);
  }
}



// Accurate but slower frame which does hints
static int PicoFrameHintsMCD(void)
{
  struct PicoVideo *pv=&Pico.video;
  int total_z80=0,lines,y,lines_vis = 224,z80CycleAim = 0,line_sample,counter75hz_lim;
  const int cycles_68k=488,cycles_z80=228,cycles_s68k=795; // both PAL and NTSC compile to same values
  int skip=PicoSkipFrame || (PicoOpt&0x10);
  int hint; // Hint counter

  if(Pico.m.pal) { //
    //cycles_68k = (int) ((double) OSC_PAL  /  7 / 50 / 312 + 0.4); // should compile to a constant (488)
    //cycles_z80 = (int) ((double) OSC_PAL  / 15 / 50 / 312 + 0.4); // 228
    lines  = 312;    // Steve Snake says there are 313 lines, but this seems to also work well
    line_sample = 68;
    counter75hz_lim = 2080;
    if(pv->reg[1]&8) lines_vis = 240;
  } else {
    //cycles_68k = (int) ((double) OSC_NTSC /  7 / 60 / 262 + 0.4); // 488
    //cycles_z80 = (int) ((double) OSC_NTSC / 15 / 60 / 262 + 0.4); // 228
    lines  = 262;
    counter75hz_lim = 2096;
    line_sample = 93;
  }

  SekCyclesReset();
  SekCyclesResetS68k();
  //z80ExtraCycles = 0;

  if(PicoOpt&4)
    z80CycleAim = 0;
//    z80_resetCycles();

  pv->status&=~0x88; // clear V-Int, come out of vblank

  hint=pv->reg[10]; // Load H-Int counter
  //dprintf("-hint: %i", hint);

  for (y=0;y<lines;y++)
  {
    Pico.m.scanline=(short)y;

    // pad delay (for 6 button pads)
    if(PicoOpt&0x20) {
      if(Pico.m.padDelay[0]++ > 25) Pico.m.padTHPhase[0]=0;
      if(Pico.m.padDelay[1]++ > 25) Pico.m.padTHPhase[1]=0;
    }

    check_cd_dma();

    // H-Interrupts:
    if(y <= lines_vis && --hint < 0) // y <= lines_vis: Comix Zone, Golden Axe
    {
      //dprintf("rhint:old @ %06x", SekPc);
      hint=pv->reg[10]; // Reload H-Int counter
      pv->pending_ints|=0x10;
      if (pv->reg[0]&0x10) SekInterrupt(4);
      //dprintf("rhint: %i @ %06x [%i|%i]", hint, SekPc, y, SekCycleCnt);
      //dprintf("hint_routine: %x", (*(unsigned short*)(Pico.ram+0x0B84)<<16)|*(unsigned short*)(Pico.ram+0x0B86));
    }

    // V-Interrupt:
    if (y == lines_vis)
    {
      //dprintf("vint: @ %06x [%i|%i]", SekPc, y, SekCycleCnt);
      pv->status|=0x88; // V-Int happened, go into vblank
      SekRun(128); SekCycleAim-=128; // there must be a gap between H and V ints, also after vblank bit set (Mazin Saga, Bram Stoker's Dracula)
      /*if(Pico.m.z80Run && (PicoOpt&4)) {
        z80CycleAim+=cycles_z80/2;
        total_z80+=z80_run(z80CycleAim-total_z80);
        z80CycleAim-=cycles_z80/2;
      }*/
      pv->pending_ints|=0x20;
      if(pv->reg[1]&0x20) SekInterrupt(6);
      if(Pico.m.z80Run && (PicoOpt&4)) // ?
        z80_int();
      //dprintf("zint: [%i|%i] zPC=%04x", Pico.m.scanline, SekCyclesDone(), mz80GetRegisterValue(NULL, 0));
    }

    // decide if we draw this line
#if CAN_HANDLE_240_LINES
    if(!skip && ((!(pv->reg[1]&8) && y<224) || ((pv->reg[1]&8) && y<240)) )
#else
    if(!skip && y<224)
#endif
      PicoLine(y);

    if(PicoOpt&1)
      sound_timers_and_dac(y);

    // get samples from sound chips
    if(y == 32 && PsndOut)
      emustatus &= ~1;
    else if((y == 224 || y == line_sample) && PsndOut)
      getSamples(y);

    // Run scanline:
      //dprintf("m68k starting exec @ %06x", SekPc);
    if(Pico.m.dma_bytes) SekCycleCnt+=CheckDMA();
    SekRun(cycles_68k);
    if ((Pico_mcd->m.busreq&3) == 1) { // no busreq/no reset
#if 0
	    int i;
	    FILE *f = fopen("prg_ram.bin", "wb");
	    for (i = 0; i < 0x80000; i+=2)
	    {
		    int tmp = Pico_mcd->prg_ram[i];
		    Pico_mcd->prg_ram[i] = Pico_mcd->prg_ram[i+1];
		    Pico_mcd->prg_ram[i+1] = tmp;
	    }
	    fwrite(Pico_mcd->prg_ram, 1, 0x80000, f);
	    fclose(f);
	    exit(1);
#endif
      //dprintf("s68k starting exec @ %06x", SekPcS68k);
      SekRunS68k(cycles_s68k);
    }

    if((PicoOpt&4) && Pico.m.z80Run) {
      Pico.m.z80Run|=2;
      z80CycleAim+=cycles_z80;
      total_z80+=z80_run(z80CycleAim-total_z80);
    }

    if ((counter75hz+=10) >= counter75hz_lim) {
      counter75hz -= counter75hz_lim;
      Check_CD_Command();
    }

    if (Pico_mcd->rot_comp.Reg_58 & 0x8000)
      gfx_cd_update();
  }

  // draw a frame just after vblank in alternative render mode
  if(!PicoSkipFrame && (PicoOpt&0x10))
    PicoFrameFull();

  return 0;
}


int PicoFrameMCD(void)
{
  if(!(PicoOpt&0x10))
    PicoFrameStart();

  PicoFrameHintsMCD();

  return 0;
}

