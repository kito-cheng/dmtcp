/*****************************************************************************
 *   Copyright (C) 2006-2008 by Michael Rieker, Jason Ansel, Kapil Arya, and *
 *                                                            Gene Cooperman *
 *   mrieker@nii.net, jansel@csail.mit.edu, kapil@ccs.neu.edu, and           *
 *                                                          gene@ccs.neu.edu *
 *                                                                           *
 *   This file is part of the MTCP module of DMTCP (DMTCP:mtcp).             *
 *                                                                           *
 *  DMTCP:mtcp is free software: you can redistribute it and/or              *
 *  modify it under the terms of the GNU Lesser General Public License as    *
 *  published by the Free Software Foundation, either version 3 of the       *
 *  License, or (at your option) any later version.                          *
 *                                                                           *
 *  DMTCP:dmtcp/src is distributed in the hope that it will be useful,       *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of           *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the            *
 *  GNU Lesser General Public License for more details.                      *
 *                                                                           *
 *  You should have received a copy of the GNU Lesser General Public         *
 *  License along with DMTCP:dmtcp/src.  If not, see                         *
 *  <http://www.gnu.org/licenses/>.                                          *
 *****************************************************************************/

/*****************************************************************************
 *
 *  Just like mmap, except it fails (EBUSY) if you do a MAP_FIXED and the space
 *  is already (partially) occupied
 *  The regular mmap will unmap anything that was there and overlay it with the
 *  new stuff
 *
 *****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/types.h>

#include "mtcp_internal.h"
#include "mtcp_util.h"

/* This goes with mtcp_sys.h.  We allocate it here in mtcp_safemmap.c,
 * because any object using mtcp_sys.h should also be
 * linking with mtcp_safemmap.c.
 */
int mtcp_sys_errno;

__attribute__ ((visibility ("hidden")))
void * mtcp_safemmap (void *start, size_t length, int prot, int flags, int fd,
                      off_t offset)
{
  int mapsfd;
  Area area;

  /* If mapping to a fixed address, make sure there's nothing there now */
  if (flags & MAP_FIXED) {
    /* Error check by scanning through the mappings of this process */
    mapsfd = mtcp_sys_open("/proc/self/maps", O_RDONLY, 0);
    if (mapsfd < 0)
      return (MAP_FAILED);
    /* Read from /proc/self/maps */
    while (mtcp_readmapsline (mapsfd, &area, NULL)) {
      /* If overlaps with what caller is trying to map, fail */
      if ((VA)start + length > area.addr &&
          (VA)start < area.addr + area.size) {
        mtcp_sys_close(mapsfd);
        mtcp_sys_errno = EBUSY;
        return (MAP_FAILED);
      }
    }
    mtcp_sys_close(mapsfd);
  }
  /* Error check succeeded; go ahead and mmap */
  return mtcp_sys_mmap (start, length, prot, flags, fd, offset);
}
