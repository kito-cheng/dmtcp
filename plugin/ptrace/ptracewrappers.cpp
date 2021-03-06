/*****************************************************************************
 *   Copyright (C) 2008-2012 by Ana-Maria Visan, Kapil Arya, and             *
 *                                                            Gene Cooperman *
 *   amvisan@cs.neu.edu, kapil@cs.neu.edu, and gene@ccs.neu.edu              *
 *                                                                           *
 *   This file is part of the PTRACE plugin of DMTCP (DMTCP:mtcp).           *
 *                                                                           *
 *  DMTCP:mtcp is free software: you can redistribute it and/or              *
 *  modify it under the terms of the GNU Lesser General Public License as    *
 *  published by the Free Software Foundation, either version 3 of the       *
 *  License, or (at your option) any later version.                          *
 *                                                                           *
 *  DMTCP:plugin/ptrace is distributed in the hope that it will be useful,   *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of           *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the            *
 *  GNU Lesser General Public License for more details.                      *
 *                                                                           *
 *  You should have received a copy of the GNU Lesser General Public         *
 *  License along with DMTCP:dmtcp/src.  If not, see                         *
 *  <http://www.gnu.org/licenses/>.                                          *
 *****************************************************************************/

#ifndef _GNU_SOURCE
# define _GNU_SOURCE /* Needed for syscall declaration */
#endif
#define _XOPEN_SOURCE 500 /* _XOPEN_SOURCE >= 500 needed for getsid */
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sched.h>
#include <sys/user.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>

#include "ptrace.h"
#include "ptraceinfo.h"
#include "dmtcpplugin.h"
#include "jassert.h"
#include "jfilesystem.h"
#include "util.h"

// Matchup this definition with the one in dmtcp/src/constants.h
#define DMTCP_FAKE_SYSCALL 1023

#define EFLAGS_OFFSET (64)
#ifdef __x86_64__
# define AX_REG rax
# define ORIG_AX_REG orig_rax
# define SP_REG rsp
# define IP_REG rip
# define SIGRETURN_INST_16 0x050f
#else
# define AX_REG eax
# define ORIG_AX_REG orig_eax
# define SP_REG esp
# define IP_REG eip
# define SIGRETURN_INST_16 0x80cd
#endif

static const unsigned char DMTCP_SYS_sigreturn =  0x77;
static const unsigned char DMTCP_SYS_rt_sigreturn = 0xad;
static const unsigned char linux_syscall[] = { 0xcd, 0x80 };

static void ptrace_detach_user_threads ();
static void ptrace_attach_threads(int isRestart);
static void ptrace_wait_for_inferior_to_reach_syscall(pid_t inf, int sysno);
static void ptrace_single_step_thread(dmtcp::Inferior *infInfo, int isRestart);
static PtraceProcState procfs_state(int tid);

extern "C" int dmtcp_is_ptracing()
{
  return dmtcp::PtraceInfo::instance().isPtracing();
}

void ptrace_process_pre_suspend_user_thread()
{
  if (dmtcp::PtraceInfo::instance().isPtracing()) {
    ptrace_detach_user_threads();
  }
}

void ptrace_process_resume_user_thread(int isRestart)
{
  if (dmtcp::PtraceInfo::instance().isPtracing()) {
    ptrace_attach_threads(isRestart);
  }
  JTRACE("Waiting for Sup Attach") (GETTID());
  dmtcp::PtraceInfo::instance().waitForSuperiorAttach();
  JTRACE("Done Waiting for Sup Attach") (GETTID());
}

static void ptrace_attach_threads(int isRestart)
{
  pid_t inferior;
  int status;
  dmtcp::vector<pid_t> inferiors;
  dmtcp::Inferior *inf;

  inferiors = dmtcp::PtraceInfo::instance().getInferiorVector(GETTID());
  if (inferiors.size() == 0) {
    return;
  }

  JTRACE("Attaching to inferior threads") (GETTID()) (inferiors.size());

  // Attach to all inferior user threads.
  for (size_t i = 0; i < inferiors.size(); i++) {
    inferior = inferiors[i];
    inf = dmtcp::PtraceInfo::instance().getInferior(inferiors[i]);
    JASSERT(inf->state() != PTRACE_PROC_INVALID) (GETTID()) (inferior);
    if (!inf->isCkptThread()) {
      JASSERT(_real_ptrace(PTRACE_ATTACH, inferior, 0, 0) != -1)
        (GETTID()) (inferior) (JASSERT_ERRNO);
      JASSERT(_real_wait4(inferior, &status, __WALL, NULL) != -1)
        (inferior) (JASSERT_ERRNO);
      JASSERT(_real_ptrace(PTRACE_SETOPTIONS, inferior, 0,
                           inf->getPtraceOptions()) != -1)
        (GETTID()) (inferior) (inf->getPtraceOptions()) (JASSERT_ERRNO);

      // Run all user threads until the end of syscall(DMTCP_FAKE_SYSCALL)
      dmtcp::PtraceInfo::instance().processPreResumeAttach(inferior);
      ptrace_wait_for_inferior_to_reach_syscall(inferior, DMTCP_FAKE_SYSCALL);
    }
  }

  // Attach to and run all user ckpthreads until the end of syscall(DMTCP_FAKE_SYSCALL)
  for (size_t i = 0; i < inferiors.size(); i++) {
    inf = dmtcp::PtraceInfo::instance().getInferior(inferiors[i]);
    inferior = inferiors[i];
    if (inf->isCkptThread()) {
      JASSERT(_real_ptrace(PTRACE_ATTACH, inferior, 0, 0) != -1)
        (GETTID()) (inferior) (JASSERT_ERRNO);
      JASSERT(_real_wait4(inferior, &status, __WALL, NULL) != -1)
        (inferior) (JASSERT_ERRNO);
      JASSERT(_real_ptrace(PTRACE_SETOPTIONS, inferior, 0,
                           inf->getPtraceOptions()) != -1)
        (GETTID()) (inferior) (inf->getPtraceOptions()) (JASSERT_ERRNO);

      // Wait for all inferiors to execute dummy syscall 'DMTCP_FAKE_SYSCALL'.
      dmtcp::PtraceInfo::instance().processPreResumeAttach(inferior);
      ptrace_wait_for_inferior_to_reach_syscall(inferior, DMTCP_FAKE_SYSCALL);
    }
  }

  // Singlestep all user threads out of the signal handler
  for (size_t i = 0; i < inferiors.size(); i++) {
    inferior = inferiors[i];
    inf = dmtcp::PtraceInfo::instance().getInferior(inferiors[i]);
    int lastCmd = inf->lastCmd();
    if (!inf->isCkptThread()) {
      /* After attach, the superior needs to singlestep the inferior out of
       * stopthisthread, aka the signal handler. */
      ptrace_single_step_thread(inf, isRestart);
      if (inf->isStopped() && (lastCmd == PTRACE_CONT ||
                                        lastCmd == PTRACE_SYSCALL)) {
        JASSERT(_real_ptrace(lastCmd, inferior, 0, 0) != -1)
          (GETTID()) (inferior) (JASSERT_ERRNO);
      }
    }
  }

  // Move ckpthreads to next step (depending on state)
  for (size_t i = 0; i < inferiors.size(); i++) {
    inferior = inferiors[i];
    inf = dmtcp::PtraceInfo::instance().getInferior(inferiors[i]);
    int lastCmd = inf->lastCmd();
    if (inf->isCkptThread() && !inf->isStopped() &&
        (lastCmd == PTRACE_CONT || lastCmd == PTRACE_SYSCALL)) {
      JASSERT(_real_ptrace(lastCmd, inferior, 0, 0) != -1)
        (GETTID()) (inferior) (JASSERT_ERRNO);
    }
  }

  JTRACE("thread done") (GETTID());
}

static void ptrace_wait_for_inferior_to_reach_syscall(pid_t inferior, int sysno)
{
  struct user_regs_struct regs;
  int syscall_number;
  int status;
  int count = 0;
  while (1) {
    count ++;
    JASSERT(_real_ptrace(PTRACE_SYSCALL, inferior, 0, 0) == 0)
      (inferior) (JASSERT_ERRNO);
    JASSERT(_real_wait4(inferior, &status, __WALL, NULL) == inferior)
      (inferior) (JASSERT_ERRNO);

    JASSERT(_real_ptrace(PTRACE_GETREGS, inferior, 0, &regs) == 0)
      (inferior) (JASSERT_ERRNO);

    syscall_number = regs.ORIG_AX_REG;
    if (syscall_number == sysno) {
      JASSERT(_real_ptrace(PTRACE_SYSCALL, inferior, 0, (void*) 0) == 0)
        (inferior) (JASSERT_ERRNO);
      JASSERT(_real_wait4(inferior, &status, __WALL, NULL) == inferior)
        (inferior) (JASSERT_ERRNO);
      break;
    }
  }
  return;
}

static void ptrace_single_step_thread(dmtcp::Inferior *inferiorInfo,
                                      int isRestart)
{
  struct user_regs_struct regs;
  long peekdata;
  unsigned long addr;
  unsigned long int eflags;

  pid_t inferior = inferiorInfo->tid();
  pid_t superior = GETTID();
  int last_command = inferiorInfo->lastCmd();
  char inferior_st = inferiorInfo->state();

  while(1) {
    int status;
    JASSERT(_real_ptrace(PTRACE_SINGLESTEP, inferior, 0, 0) != -1)
      (superior) (inferior) (JASSERT_ERRNO);
    if (_real_wait4(inferior, &status, 0, NULL) == -1) {
      JASSERT(_real_wait4(inferior, &status, __WCLONE, NULL) != -1)
        (superior) (inferior) (JASSERT_ERRNO);
    }
    if (WIFEXITED(status)) {
      JTRACE("thread is dead") (inferior) (WEXITSTATUS(status));
    } else if(WIFSIGNALED(status)) {
      JTRACE("thread terminated by signal") (inferior);
    }

    JASSERT(_real_ptrace(PTRACE_GETREGS, inferior, 0, &regs) != -1)
      (superior) (inferior) (JASSERT_ERRNO);
    peekdata = _real_ptrace(PTRACE_PEEKDATA, inferior, (void*) regs.IP_REG, 0);
    long inst = peekdata & 0xffff;
#ifdef __x86_64__
    /* For 64 bit architectures. */
    if (inst == SIGRETURN_INST_16 && regs.AX_REG == 0xf) {
#else /* For 32 bit architectures.*/
    if (inst == SIGRETURN_INST_16 && (regs.AX_REG == DMTCP_SYS_sigreturn ||
                                      regs.AX_REG == DMTCP_SYS_rt_sigreturn)) {
#endif
      if (isRestart) { /* Restart time. */
        // FIXME: TODO:
        if (last_command == PTRACE_SINGLESTEP) {
          if (regs.AX_REG != DMTCP_SYS_rt_sigreturn) {
            addr = regs.SP_REG;
          } else {
            addr = regs.SP_REG + 8;
            addr = _real_ptrace(PTRACE_PEEKDATA, inferior, (void*) addr, 0);
            addr += 20;
          }
          addr += EFLAGS_OFFSET;
          errno = 0;
          JASSERT ((int) (eflags = _real_ptrace(PTRACE_PEEKDATA, inferior,
                                         (void *)addr, 0)) != -1)
            (superior) (inferior) (JASSERT_ERRNO);
          eflags |= 0x0100;
          JASSERT(_real_ptrace(PTRACE_POKEDATA, inferior, (void *)addr,
                              (void*) eflags) != -1)
            (superior) (inferior) (JASSERT_ERRNO);
        } else if (inferior_st != PTRACE_PROC_TRACING_STOP) {
          /* TODO: remove in future as GROUP restore becames stable
           *                                                    - Artem */
          JASSERT(_real_ptrace(PTRACE_CONT, inferior, 0, 0) != -1)
            (superior) (inferior) (JASSERT_ERRNO);
        }
      } else { /* Resume time. */
        if (inferior_st != PTRACE_PROC_TRACING_STOP) {
          JASSERT(_real_ptrace(PTRACE_CONT, inferior, 0, 0) != -1)
            (superior) (inferior) (JASSERT_ERRNO);
        }
      }

      /* In case we have checkpointed at a breakpoint, we don't want to
       * hit the same breakpoint twice. Thus this code. */
      // TODO: FIXME: Replace this code with a raise(SIGTRAP) and see what happens
      if (inferior_st == PTRACE_PROC_TRACING_STOP) {
        JASSERT(_real_ptrace(PTRACE_SINGLESTEP, inferior, 0, 0) != -1)
          (superior) (inferior) (JASSERT_ERRNO);
        if (_real_wait4(inferior, &status, 0, NULL) == -1) {
          JASSERT(_real_wait4(inferior, &status, __WCLONE, NULL) != -1)
            (superior) (inferior) (JASSERT_ERRNO);
        }
      }
      break;
    }
  } //while(1)
}

/* This function detaches the user threads. */
static void ptrace_detach_user_threads ()
{
  PtraceProcState pstate;
  int status;
  struct rusage rusage;
  dmtcp::vector<pid_t> inferiors;
  dmtcp::Inferior *inf;

  inferiors = dmtcp::PtraceInfo::instance().getInferiorVector(GETTID());

  for (size_t i = 0; i < inferiors.size(); i++) {
    pid_t inferior = inferiors[i];
    inf = dmtcp::PtraceInfo::instance().getInferior(inferiors[i]);
    void *data = (void*) (unsigned long) dmtcp_get_ckpt_signal();
    pstate = procfs_state(inferiors[i]);
    if (pstate == PTRACE_PROC_INVALID) {
      JTRACE("Inferior does not exist.") (inferior);
      dmtcp::PtraceInfo::instance().eraseInferior(inferior);
      continue;
    }
    inf->setState(pstate);
    inf->semInit();

    if (inf->isCkptThread()) {
      data = NULL;
    }
    int ret = _real_wait4(inferior, &status, __WALL | WNOHANG, &rusage);
    if (ret > 0) {
      if (!WIFSTOPPED(status) || WSTOPSIG(status) != dmtcp_get_ckpt_signal()) {
        inf->setWait4Status(&status, &rusage);
      }
    }
    pstate = procfs_state(inferiors[i]);
    if (pstate == PTRACE_PROC_RUNNING || pstate == PTRACE_PROC_SLEEPING) {
      syscall(SYS_tkill, inferior, SIGSTOP);
      _real_wait4(inferior, &status, __WALL, NULL);
      JASSERT(_real_wait4(inferior, &status, __WALL | WNOHANG, NULL) == 0)
        (inferior) (JASSERT_ERRNO);
    }
    if (_real_ptrace(PTRACE_DETACH, inferior, 0, data) == -1) {
      JASSERT(errno == ESRCH)
        (GETTID()) (inferior) (JASSERT_ERRNO);
      dmtcp::PtraceInfo::instance().eraseInferior(inferior);
      continue;
    }
    pstate = procfs_state(inferiors[i]);
    if (pstate == PTRACE_PROC_STOPPED) {
      kill(inferior, SIGCONT);
    }
    JTRACE("Detached thread") (inferior);
  }
}

static PtraceProcState procfs_state(int pid)
{
  int fd;
  char buf[512];
  char *str;
  const char *key = "State:";
  int len = strlen(key);

  snprintf (buf, sizeof (buf), "/proc/%d/status", (int) pid);
  fd = _real_open (buf, O_RDONLY, 0);
  if (fd < 0) {
    JTRACE("open() failed") (buf);
    return PTRACE_PROC_INVALID;
  }


  dmtcp::Util::readAll(fd, buf, sizeof buf);
  close(fd);
  str = strstr(buf, key);
  JASSERT(str != NULL);
  str += len;

  while (*str == ' ' || *str == '\t') {
    str++;
  }

  if (strcasestr(str, "T (stopped)") != NULL) {
    return PTRACE_PROC_STOPPED;
  } else if (strcasestr(str, "T (tracing stop)") != NULL) {
    return PTRACE_PROC_TRACING_STOP;
  } else if (strcasestr(str, "S (sleeping)") != NULL ||
             strcasestr(str, "D (disk sleep)") != NULL) {
    return PTRACE_PROC_SLEEPING;
  } else if (strcasestr(str, "R (running)") != NULL) {
    return PTRACE_PROC_RUNNING;
  }
  return PTRACE_PROC_UNDEFINED;
}


/*****************************************************************************
 ****************************************************************************/

extern "C" pid_t waitpid(pid_t pid, int *stat, int options)
{
  return wait4(pid, stat, options, NULL);
}

extern "C" pid_t wait4(pid_t pid, void *stat, int options,
                       struct rusage *rusage)
{
  int status;
  struct rusage rusagebuf;
  pid_t retval;
  int *stat_loc = (int*) stat;
  bool repeat = false;

  if (stat_loc == NULL) {
    stat_loc = &status;
  }

  if (rusage == NULL) {
    rusage = &rusagebuf;
  }

  retval = dmtcp::PtraceInfo::instance().getWait4Status(pid, stat_loc, rusage);
  if (retval != -1) {
    return retval;
  }

  do {
    retval = _real_wait4(pid, stat_loc, options, rusage);
    DMTCP_PLUGIN_DISABLE_CKPT();
    if (retval > 0 && dmtcp::PtraceInfo::instance().isInferior(retval)) {
      if (WIFSTOPPED(*stat_loc) && WSTOPSIG(*stat_loc) == dmtcp_get_ckpt_signal()) {
        /* Inferior got STOPSIGNAL, this should not be passed to gdb process as
         * we are performing checkpoint at this time. We should reexecute the
         * _real_wait4 to get the status that the gdb process would want to
         * process.
         */
        repeat = true;
      } else if (WIFSTOPPED(*stat_loc)) {
        dmtcp::PtraceInfo::instance().setLastCmd(retval, -1);
      } else if (WIFEXITED(*stat_loc) || WIFSIGNALED(*stat_loc)) {
        dmtcp::PtraceInfo::instance().eraseInferior(retval);
      }
    }
    DMTCP_PLUGIN_ENABLE_CKPT();
  } while (repeat);

  return retval;
}

extern "C" long ptrace (enum __ptrace_request request, ...)
{
  va_list ap;
  pid_t pid;
  void *addr;
  void *data;

  va_start(ap, request);
  pid = va_arg(ap, pid_t);
  addr = va_arg(ap, void *);
  data = va_arg(ap, void *);
  va_end(ap);

  DMTCP_PLUGIN_DISABLE_CKPT();
  dmtcp::PtraceInfo::instance().setPtracing();

  long ptrace_ret =  _real_ptrace(request, pid, addr, data);

  if (ptrace_ret != -1) {
    dmtcp::PtraceInfo::instance().processSuccessfulPtraceCmd(request, pid,
                                                             addr, data);
  }

  DMTCP_PLUGIN_ENABLE_CKPT();
  return ptrace_ret;
}
