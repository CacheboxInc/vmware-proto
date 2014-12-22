/*
 * Copyright(2013) Cachebox Inc.
 *
 * utils.h
 */

#ifndef UTILS_H
#define UTILS_H
#include <sys/types.h>
#include <inttypes.h>

/* Report size of disk/file in bytes. */
off_t getsizedev(char *path);

/* find the first prime number larger than the given num */
uint64_t next_prime(int num);


#endif /* end of UTILS_H */
