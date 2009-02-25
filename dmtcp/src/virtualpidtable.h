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

#include <sys/types.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <iostream>
#include <map>
#include  "../jalib/jserialize.h"
#include "uniquepid.h"
#include "constants.h"

#ifndef VIRTUAL_PID_TABLE_H
#define VIRTUAL_PID_TABLE_H

#ifdef PID_VIRTUALIZATION
namespace dmtcp
{
  /* Shall we create seperate classes for holding original to current pid map
   * and  for holding child process ids?
   */

  class VirtualPidTable
  {
    public:
      VirtualPidTable();
      static VirtualPidTable& Instance();
      void postRestart();
      pid_t originalToCurrentPid( pid_t originalPid );
      pid_t currentToOriginalPid( pid_t currentPid );
      void  insert(pid_t originalPid,  dmtcp::UniquePid uniquePid);
      //void  insertTid(pid_t tid) { _tids.pushback(tid); }
      void  erase(pid_t originalPid);
      void serialize ( jalib::JBinarySerializer& o );
      void serializePidMap ( jalib::JBinarySerializer& o );

      void setRootOfProcessTree() { _isRootOfProcessTree = true; }
      bool isRootOfProcessTree() const { return _isRootOfProcessTree; }
      void updateRootOfProcessTree();

      std::vector< pid_t > getPidVector();
      bool pidExists( pid_t pid );

      typedef std::map< pid_t , dmtcp::UniquePid >::iterator iterator;
      iterator begin() { return _childTable.begin(); }
      iterator end() { return _childTable.end(); }

      pid_t pid() const { return _pid; }
      pid_t ppid() const { return _ppid; }

      void setppid( pid_t ppid ) { _ppid = ppid; }

      void updateMapping (pid_t originalPid, pid_t currentPid);

      void resetOnFork();

    protected:

    private:
      std::map< pid_t , dmtcp::UniquePid > _childTable;
      typedef std::map< pid_t , pid_t >::iterator pid_iterator;
      std::map< pid_t , pid_t > _pidMapTable;

      //std::vector< pid_t > _tids;
      //typedef std::vector< pid_t >::iterator tid_iterator;

      bool  _isRootOfProcessTree;
      pid_t _pid;
      pid_t _ppid;
  };

}

#endif /* PID_VIRTUALIZATION */
#endif