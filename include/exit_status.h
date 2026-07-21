#ifndef CUP_EXIT_STATUS_H
#define CUP_EXIT_STATUS_H

/* Stable public process statuses independent of the internal CupError enum. */

#include "error.h"

enum {
    CUP_STATUS_SUCCESS = 0,
    CUP_STATUS_USAGE = 2,
    CUP_STATUS_UNAVAILABLE = 3,
    CUP_STATUS_STATE = 4,
    CUP_STATUS_NETWORK = 5,
    CUP_STATUS_OPERATION = 6,
    CUP_STATUS_INTERNAL = 70,
    CUP_STATUS_INTERRUPT = 130
};

/* Collapse detailed internal failures into the stable CLI status set above. */
int cup_error_to_exit_status(CupError error);

#endif /* CUP_EXIT_STATUS_H */
