#ifndef CUP_INTERRUPT_H
#define CUP_INTERRUPT_H

/* Install, remove and query the process interruption handler. */
void interrupt_setup(void);
void interrupt_reset(void);
int interrupt_requested(void);

#endif /* CUP_INTERRUPT_H */
