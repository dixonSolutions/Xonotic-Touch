#ifndef TOUCH_PRVM_H
#define TOUCH_PRVM_H

// Godnotic: QuakeC VM load-time helpers (touch_prvm.c).
struct prvm_prog_s;
// prvm_edict.c PRVM_MEM_IncreaseEdicts: the size to grow to instead of newmax
int TouchPRVM_EdictGrowth(struct prvm_prog_s *prog, int newmax);
void TouchPRVM_Init(void);

#endif
