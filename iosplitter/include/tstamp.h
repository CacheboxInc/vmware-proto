/*
 * tstamp.h
 */
#ifndef TSTAMP_H
#define TSTAMP_H

#include "cdevtypes.h"

#define TSTAMP_CLOCKID CLOCK_MONOTONIC

/*
 * tstamp_t counts in milliseconds.
 */
tstamp_t tstamp_now(void);

#endif /*TSTAMP_H*/
