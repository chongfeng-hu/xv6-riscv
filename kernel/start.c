#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

void main();
void timerinit();

// entry.S needs one stack per CPU.
__attribute__ ((aligned (16))) char stack0[4096 * NCPU];

// a scratch area per CPU for machine-mode timer interrupts.
uint64 timer_scratch[NCPU][5];

// assembly code in kernelvec.S for machine-mode timer interrupt.
extern void timervec();

// entry.S jumps here in machine mode on stack0.
void
start()
{
  // set M Previous Privilege mode to Supervisor, for mret.
  unsigned long x = r_mstatus();
  x &= ~MSTATUS_MPP_MASK;
  x |= MSTATUS_MPP_S;
  w_mstatus(x);

  // set M Exception Program Counter to main, for mret.
  // requires gcc -mcmodel=medany
  w_mepc((uint64)main);

  // disable paging for now.
  w_satp(0);

  // delegate all interrupts and exceptions to supervisor mode.
  //
  // note that medeleg and mideleg CSRs are WARL. for mideleg, following is the
  // behavior:
  //
  // interrupt  can-delegate  reason-code
  // =========  ============  ===========
  // USI        N             U
  // SSI        Y             S
  // MSI        N             M
  // UTI        N             U
  // STI        Y             S
  // MTI        N             M
  // UEI        N             U
  // SEI        Y             S
  // MEI        N             M
  //
  // reason U: setting a bit here means that a U-mode interrupt will be
  // delegated to the S-mode interrupt handler. a U-mode interrupt can only
  // occur in U-mode, but qemu doesn't support U-mode interrupts, and thus this
  // delegation is not meaningful.
  //
  // reason S: setting a bit here means that a S-mode interrupt will be
  // delegated to the S-mode interrupt handler (instead of the default M-mode
  // interrupt handler).
  //
  // reason M: setting a bit here means that a M-mode interrupt will be
  // delegated to the S-mode interrupt handler, but a higher level interrupt can
  // never be handled by a lower level interrupt handler, and thus this
  // delegation is not meaningful.
  w_medeleg(0xffff);
  w_mideleg(0xffff);
  w_sie(r_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);

  // configure Physical Memory Protection to give supervisor mode
  // access to all of physical memory.
  w_pmpaddr0(0x3fffffffffffffull);
  w_pmpcfg0(0xf);

  // ask for clock interrupts.
  timerinit();

  // keep each CPU's hartid in its tp register, for cpuid().
  int id = r_mhartid();
  w_tp(id);

  // switch to supervisor mode and jump to main().
  asm volatile("mret");
}

// arrange to receive timer interrupts.
// they will arrive in machine mode at
// at timervec in kernelvec.S,
// which turns them into software interrupts for
// devintr() in trap.c.
void
timerinit()
{
  // each CPU has a separate source of timer interrupts.
  int id = r_mhartid();

  // ask the CLINT for a timer interrupt.
  int interval = 1000000; // cycles; about 1/10th second in qemu.
  *(uint64*)CLINT_MTIMECMP(id) = *(uint64*)CLINT_MTIME + interval;

  // prepare information in scratch[] for timervec.
  // scratch[0..2] : space for timervec to save registers.
  // scratch[3] : address of CLINT MTIMECMP register.
  // scratch[4] : desired interval (in cycles) between timer interrupts.
  uint64 *scratch = &timer_scratch[id][0];
  scratch[3] = CLINT_MTIMECMP(id);
  scratch[4] = interval;
  w_mscratch((uint64)scratch);

  // set the machine-mode trap handler.
  w_mtvec((uint64)timervec);

  // enable machine-mode interrupts.
  //
  // note this setting only affects the M-mode. when running in S-mode or
  // U-mode, M-mode interrupts are always enabled, regardless of the MIE
  // setting. quote from spec:
  //
  // When a hart is executing in privilege mode x, interrupts are globally
  // enabled when xIE=1 and globally disabled when xIE=0. Interrupts for
  // lower-privilege modes, w<x, are always globally disabled regardless of the
  // setting of the lower-privilege mode’s global wIE bit. Interrupts for
  // higher-privilege modes, y>x, are always globally enabled regardless of the
  // setting of the higherprivilege mode’s global yIE bit.
  w_mstatus(r_mstatus() | MSTATUS_MIE);

  // enable machine-mode timer interrupts.
  w_mie(r_mie() | MIE_MTIE);
}
