/*
 * Copyright(2013) Cachebox Inc.
 *
 * utils.c
 * generic utilities
 */

#include "utils.h"
#include <sys/utsname.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <unistd.h>

/* get the size of a device. 
 * got this from Prasad, need to very source TBD */

off_t 
getsizedev(char *path)
{
    struct utsname ut;
    unsigned long long numbytes;
    int valid_blkgetsize64 = 1;
    int fd;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        exit(1);
    }

    /* Ref: e2fsprogs-1.39 - apparently BLKGETSIZE64 doesn't work pre 2.6 */
    if ((uname(&ut) == 0) &&
            ((ut.release[0] == '2') && (ut.release[1] == '.') &&
             (ut.release[2] < '6') && (ut.release[3] == '.')))
                valid_blkgetsize64 = 0;
    if (valid_blkgetsize64) {
        if (ioctl(fd, BLKGETSIZE64, &numbytes) < 0) {
            fprintf(stderr, "ioctl BLKGETSIZE64 %s: %s\n", path,
                    strerror(errno));
            exit(1);
        }
    } else {
        unsigned long numblocks;

        if (ioctl(fd, BLKGETSIZE, &numblocks) < 0) {
            fprintf(stderr, "ioctl BLKGETSIZE %s: %s\n", path,
                    strerror(errno));
            exit(1);
        }
        numbytes = (off_t)numblocks*512; /* 2TB limit here */
    }

    (void)close(fd);

    return (off_t)numbytes;
}
