#ifndef CUP_INTERRUPT_H
#define CUP_INTERRUPT_H

void interrupt_setup(void);
void interrupt_reset(void);
int interrupt_requested(void);

#endif /* CUP_INTERRUPT_H */