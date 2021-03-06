/****************************************************************************
 *   Copyright (C) 2006-2008 by Jason Ansel, Kapil Arya, and Gene Cooperman *
 *   jansel@csail.mit.edu, kapil@ccs.neu.edu, gene@ccs.neu.edu              *
 *                                                                          *
 *   This file is part of the dmtcp/src module of DMTCP (DMTCP:dmtcp/src).  *
 *                                                                          *
 *  DMTCP:dmtcp/src is free software: you can redistribute it and/or        *
 *  modify it under the terms of the GNU Lesser General Public License as   *
 *  published by the Free Software Foundation, either version 3 of the      *
 *  License, or (at your option) any later version.                         *
 *                                                                          *
 *  DMTCP:dmtcp/src is distributed in the hope that it will be useful,      *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 *  GNU Lesser General Public License for more details.                     *
 *                                                                          *
 *  You should have received a copy of the GNU Lesser General Public        *
 *  License along with DMTCP:dmtcp/src.  If not, see                        *
 *  <http://www.gnu.org/licenses/>.                                         *
 ****************************************************************************/

#ifndef DMTCPPROTECTEDFDS_H
#define DMTCPPROTECTEDFDS_H


#include "../jalib/jalloc.h"

#define PROTECTEDFDS (dmtcp::ProtectedFDs::instance())
#define PFD(i) (PROTECTED_FD_START + (i))
//#define PROTECTEDFD(i) PFD(i)
#define PROTECTED_COORD_FD         PFD(1)
#define PROTECTED_VIRT_COORD_FD    PFD(2)
#define PROTECTED_RESTORE_SOCK_FD  PFD(3)
#define PROTECTED_COORD_ALT_FD     PFD(4)
#define PROTECTED_STDERR_FD        PFD(5)
#define PROTECTED_JASSERTLOG_FD    PFD(6)
#define PROTECTED_CKPT_FILES_FD    PFD(7)
#define PROTECTED_SHM_FD           PFD(8)
#define PROTECTED_PIDMAP_FD        PFD(9)
#define PROTECTED_PTRACE_FD        PFD(10)
#define PROTECTED_TMPDIR_FD        PFD(11)
#define PROTECTED_FILE_FDREWIRER_FD     PFD(12)
#define PROTECTED_SOCKET_FDREWIRER_FD     PFD(13)
#define PROTECTED_EVENT_FDREWIRER_FD     PFD(14)
#define PROTECTED_READLOG_FD       PFD(15)

#define PROTECTED_FD_START 820
#define PROTECTED_FD_COUNT 16

#define DMTCP_IS_PROTECTED_FD(fd) \
  (fd >= PFD(0) && fd < PFD(PROTECTED_FD_COUNT))

namespace dmtcp
{

  class ProtectedFDs
  {
    public:
#ifdef JALIB_ALLOCATOR
      static void* operator new(size_t nbytes, void* p) { return p; }
      static void* operator new(size_t nbytes) { JALLOC_HELPER_NEW(nbytes); }
      static void  operator delete(void* p) { JALLOC_HELPER_DELETE(p); }
#endif
      static ProtectedFDs& instance();
      static bool isProtected ( int fd );
    protected:
      ProtectedFDs();
    private:
//     bool _usageTable[PROTECTED_FD_COUNT];
  };

}

#endif
