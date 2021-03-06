/****************************************************************************
 *   Copyright (C) 2006-2010 by Jason Ansel, Kapil Arya, and Gene Cooperman *
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

/****************************************************************************
 * Coordinator code logic:                                                  *
 * main calls monitorSockets, which acts as a top level event loop.         *
 * monitorSockets calls:  onConnect, onData, onDisconnect, onTimeoutInterval*
 *   when client or dmtcp_command talks to coordinator.                     *
 * onConnect and onData receive a socket parameter, read msg, and pass to:  *
 *   handleUserCommand, which takes single char arg ('s', 'c', 'k', 'q', ...)*
 * handleUserCommand calls broadcastMessage to send data back               *
 * any message sent by broadcastMessage takes effect only on returning      *
 *   back up to top level monitorSockets                                    *
 * Hence, even for checkpoint, handleUserCommand just changes state,        *
 *   broadcasts an initial checkpoint command, and then returns to top      *
 *   level.  Replies from clients then drive further state changes.        *
 * The prefix command 'b' (blocking) from dmtcp_command modifies behavior   *
 *   of 'c' so that the reply to dmtcp_command happens only when clients    *
 *   are back in RUNNING state.                                             *
 * The states for a worker (client) are:                                    *
 * Checkpoint: RUNNING -> SUSPENDED -> FD_LEADER_ELECTION -> DRAINED        *
 *       	  -> CHECKPOINTED -> NAME_SERVICE_DATA_REGISTERED           *
 *                -> DONE_QUERYING -> REFILLED -> RUNNING		    *
 * Restart:    RESTARTING -> CHECKPOINTED -> NAME_SERVICE_DATA_REGISTERED   *
 *                -> DONE_QUERYING -> REFILLED -> RUNNING	            *
 * If debugging, set gdb breakpoint on:					    *
 *   dmtcp::DmtcpCoordinator::onConnect					    *
 *   dmtcp::DmtcpCoordinator::onData					    *
 *   dmtcp::DmtcpCoordinator::handleUserCommand				    *
 *   dmtcp::DmtcpCoordinator::broadcastMessage				    *
 ****************************************************************************/

#include "dmtcp_coordinator.h"
#include "constants.h"
#include "protectedfds.h"
#include "dmtcpmessagetypes.h"
#include "coordinatorapi.h"
#include "lookup_service.h"
#include "syscallwrappers.h"
#include "util.h"
#include "../jalib/jconvert.h"
#include "../jalib/jtimer.h"
#include "../jalib/jfilesystem.h"
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <algorithm>
#include <iomanip>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#undef min
#undef max

#define BINARY_NAME "dmtcp_coordinator"

static int thePort = -1;

static const char* theHelpMessage =
  "COMMANDS:\n"
  "  l : List connected nodes\n"
  "  s : Print status message\n"
  "  c : Checkpoint all nodes\n"
  "  i : Print current checkpoint interval\n"
  "      (To change checkpoint interval, use dmtcp_command)\n"
  "  f : Force a restart even if there are missing nodes (debugging only)\n"
  "  k : Kill all nodes\n"
  "  q : Kill all nodes and quit\n"
  "  ? : Show this message\n"
  "\n"
;

static const char* theUsage =
  "USAGE: \n"
  "   dmtcp_coordinator [OPTIONS] [port]\n\n"
  "OPTIONS:\n"
  "  --port, -p, (environment variable DMTCP_PORT):\n"
  "      Port to listen on (default: 7779)\n"
  "  --ckptdir, -c, (environment variable DMTCP_CHECKPOINT_DIR):\n"
  "      Directory to store dmtcp_restart_script.sh (default: ./)\n"
  "  --tmpdir, -t, (environment variable DMTCP_TMPDIR):\n"
  "      Directory to store temporary files (default: env var TMDPIR or /tmp)\n"
  "  --exit-on-last\n"
  "      Exit automatically when last client disconnects\n"
  "  --background\n"
  "      Run silently in the background (mutually exclusive with --batch)\n"
  "  --batch\n"
  "      Run in batch mode (mutually exclusive with --background)\n"
  "      The checkpoint interval is set to 3600 seconds (1 hr) by default\n"
  "  --interval, -i, (environment variable DMTCP_CHECKPOINT_INTERVAL):\n"
  "      Time in seconds between automatic checkpoints\n"
  "      (default: 0, disabled)\n"
  "  --help:\n"
  "      Print this message and exit.\n"
  "  --version:\n"
  "      Print version information and exit.\n"
  "\n"
  "COMMANDS:\n"
  "  (type '?<return>' at runtime for list)\n"
  "\n"
  "See " PACKAGE_URL " for more information.\n"
;


static const char* theRestartScriptHeader =
  "#!/bin/bash\n\n"
  "set -m # turn on job control\n\n"
  "#This script launches all the restarts in the background.\n"
  "#Suggestions for editing:\n"
  "#  1. For those processes executing on the localhost, remove\n"
  "#     'ssh <hostname> from the start of the line. \n"
  "#  2. If using ssh, verify that ssh does not require passwords or other\n"
  "#     prompts.\n"
  "#  3. Verify that the dmtcp_restart command is in your path on all hosts,\n"
  "#     otherwise set the remote_prefix appropriately.\n"
  "#  4. Verify DMTCP_HOST and DMTCP_PORT match the location of the\n"
  "#     dmtcp_coordinator. If necessary, add\n"
  "#     'DMTCP_PORT=<dmtcp_coordinator port>' after 'DMTCP_HOST=<...>'.\n"
  "#  5. Remove the '&' from a line if that process reads STDIN.\n"
  "#     If multiple processes read STDIN then prefix the line with\n"
  "#     'xterm -hold -e' and put '&' at the end of the line.\n"
  "#  6. Processes on same host can be restarted with single dmtcp_restart\n"
  "#     command.\n\n\n"
;

static const char* theRestartScriptCheckLocal =
  "check_local()\n"
  "{\n"
  "  worker_host=$1\n"
  "  unset is_local_node\n"
  "  worker_ip=$(nslookup $worker_host | grep -A1 'Name:' | grep 'Address:' | sed -e 's/Address://' -e 's/ //' -e 's/	//')\n"
  "  ifconfig_path=`which ifconfig`\n"
  "  if [ -z \"$ifconfig_path\" ]; then\n"
  "    ifconfig_path=\"/sbin/ifconfig\"\n"
  "  fi\n"
  "  output=`$ifconfig_path -a | grep \"inet addr:.*${worker_ip}.*Bcast\"`\n"
  "  if [ -n \"$output\" ]; then\n"
  "    is_local_node=1\n"
  "  else\n"
  "    is_local_node=0\n"
  "  fi\n"
  "}\n\n\n";


static const char* theRestartScriptUsage =
  "usage_str='USAGE:\n"
  "  dmtcp_restart_script.sh [OPTIONS]\n\n"
  "OPTIONS:\n"
  "  --host, -h, (environment variable DMTCP_HOST):\n"
  "      Hostname where dmtcp_coordinator is running\n"
  "  --port, -p, (environment variable DMTCP_PORT):\n"
  "      Port where dmtcp_coordinator is running\n"
  "  --hostfile <arg0> :\n"
  "      Provide a hostfile (One host per line, \"#\" indicates comments)\n"
  "  --restartdir, -d, (environment variable DMTCP_RESTART_DIR):\n"
  "      Directory to read checkpoint images from\n"
  "  --batch, -b:\n"
  "      Enable batch mode for dmtcp_restart\n"
  "  --disable-batch, -b:\n"
  "      Disable batch mode for dmtcp_restart (if previously enabled)\n"
  "  --interval, -i, (environment variable DMTCP_CHECKPOINT_INTERVAL):\n"
  "      Time in seconds between automatic checkpoints\n"
  "      (Default: Use pre-checkpoint value)\n"
  "  --help:\n"
  "      Print this message and exit.\'\n"
  "\n\n"
;

static const char* theRestartScriptCmdlineArgHandler =
  "if [ $# -gt 0 ]; then\n"
  "  while [ $# -gt 0 ]\n"
  "  do\n"
  "    if [ $1 = \"--help\" ]; then\n"
  "      echo \"$usage_str\"\n"
  "      exit\n"
  "    elif [ $1 = \"--batch\" -o $1 = \"-b\" ]; then\n"
  "      maybebatch='--batch'\n"
  "      shift\n"
  "    elif [ $1 = \"--disable-batch\" ]; then\n"
  "      maybebatch=\n"
  "      shift\n"
  "    elif [ $# -ge 2 ]; then\n"
  "      case \"$1\" in \n"
  "        --host|-h)\n"
  "          coord_host=\"$2\";;\n"
  "        --port|-p)\n"
  "          coord_port=\"$2\";;\n"
  "        --hostfile)\n"
  "          hostfile=\"$2\"\n"
  "          if [ ! -f \"$hostfile\" ]; then\n"
  "            echo \"ERROR: hostfile $hostfile not found\"\n"
  "            exit\n"
  "          fi;;\n"
  "        --restartdir|-d)\n"
  "          DMTCP_RESTART_DIR=$2;;\n"
  "        --interval|-i)\n"
  "          checkpoint_interval=$2;;\n"
  "        *)\n"
  "          echo \"$0: unrecognized option \'$1\'. See correct usage below\"\n"
  "          echo \"$usage_str\"\n"
  "          exit;;\n"
  "      esac\n"
  "      shift\n"
  "      shift\n"
  "    elif [ $1 = \"--help\" ]; then\n"
  "      echo \"$usage_str\"\n"
  "      exit\n"
  "    else\n"
  "      echo \"$0: Incorrect usage.  See correct usage below\"\n"
  "      echo\n"
  "      echo \"$usage_str\"\n"
  "      exit\n"
  "    fi\n"
  "  done\n"
  "fi\n\n"
;

static const char* theRestartScriptSingleHostProcessing =
  "ckpt_files=\"\"\n"
  "if [ ! -z \"$DMTCP_RESTART_DIR\" ]; then\n"
  "  for tmp in $given_ckpt_files; do\n"
  "    ckpt_files=\"$DMTCP_RESTART_DIR/$(basename $tmp) $ckpt_files\"\n"
  "  done\n"
  "else\n"
  "  ckpt_files=$given_ckpt_files\n"
  "fi\n\n"

  "coordinator_info=\n"
  "if [ -z \"$maybebatch\" ]; then\n"
  "  coordinator_info=\"--host $coord_host --port $coord_port\"\n"
  "fi\n\n"

  "exec $dmt_rstr_cmd $coordinator_info\\\n"
  "  $maybebatch $maybejoin --interval \"$checkpoint_interval\"\\\n"
  "  $ckpt_files\n"
;

static const char* theRestartScriptMultiHostProcessing =
  "worker_ckpts_regexp=\\\n"
  "\'[^:]*::[ \\t\\n]*\\([^ \\t\\n]\\+\\)[ \\t\\n]*:\\([a-z]\\+\\):[ \\t\\n]*\\([^:]\\+\\)\'\n\n"

  "worker_hosts=$(\\\n"
  "  echo $worker_ckpts | sed -e \'s/\'\"$worker_ckpts_regexp\"\'/\\1 /g\')\n"
  "restart_modes=$(\\\n"
  "  echo $worker_ckpts | sed -e \'s/\'\"$worker_ckpts_regexp\"\'/: \\2/g\')\n"
  "ckpt_files_groups=$(\\\n"
  "  echo $worker_ckpts | sed -e \'s/\'\"$worker_ckpts_regexp\"\'/: \\3/g\')\n"
  "\n"
  "if [ ! -z \"$hostfile\" ]; then\n"
  "  worker_hosts=$(\\\n"
  "    cat \"$hostfile\" | sed -e \'s/#.*//\' -e \'s/[ \\t\\r]*//\' -e \'/^$/ d\')\n"
  "fi\n\n"

  "localhost_ckpt_files_group=\n\n"

  "num_worker_hosts=$(echo $worker_hosts | wc -w)\n\n"

  "maybejoin=\n"
  "if [ \"$num_worker_hosts\" != \"1\" ]; then\n"
  "  maybejoin='--join'\n"
  "fi\n\n"

  "for worker_host in $worker_hosts\n"
  "do\n\n"
  "  ckpt_files_group=$(\\\n"
  "    echo $ckpt_files_groups | sed -e \'s/[^:]*:[ \\t\\n]*\\([^:]*\\).*/\\1/\')\n"
  "  ckpt_files_groups=$(echo $ckpt_files_groups | sed -e \'s/[^:]*:[^:]*//\')\n"
  "\n"
  "  mode=$(echo $restart_modes | sed -e \'s/[^:]*:[ \\t\\n]*\\([^:]*\\).*/\\1/\')\n"
  "  restart_modes=$(echo $restart_modes | sed -e \'s/[^:]*:[^:]*//\')\n\n"
  "  maybexterm=\n"
  "  maybebg=\n"
  "  case $mode in\n"
  "    bg) maybebg=\'bg\';;\n"
  "    xterm) maybexterm=xterm;;\n"
  "    fg) ;;\n"
  "    *) echo \"WARNING: Unknown Mode\";;\n"
  "  esac\n\n"
  "  if [ -z \"$ckpt_files_group\" ]; then\n"
  "    break;\n"
  "  fi\n\n"

  "  new_ckpt_files_group=\"\"\n"
  "  for tmp in $ckpt_files_group\n"
  "  do\n"
  "      if  [ ! -z \"$DMTCP_RESTART_DIR\" ]; then\n"
  "        tmp=$DMTCP_RESTART_DIR/$(basename $tmp)\n"
  "      fi\n"
  "      new_ckpt_files_group=\"$new_ckpt_files_group $tmp\"\n"
  "  done\n\n"

  "  check_local $worker_host\n"
  "  if [ \"$is_local_node\" -eq 1 -o \"$num_worker_hosts\" == \"1\" ]; then\n"
  "    localhost_ckpt_files_group=\"$new_ckpt_files_group\"\n"
  "    continue\n"
  "  fi\n"

  "  if [ -z $maybebg ]; then\n"
  "    $maybexterm /usr/bin/ssh -t \"$worker_host\" \\\n"
  "      $remote_dmt_rstr_cmd --host \"$coord_host\" --port \"$coord_port\"\\\n"
  "      $maybebatch --join --interval \"$checkpoint_interval\"\\\n"
  "      $new_ckpt_files_group\n"
  "  else\n"
  "    $maybexterm /usr/bin/ssh \"$worker_host\" \\\n"
  // In OpenMPI 1.4, without this (sh -c ...), orterun hangs at the
  // end of the computation until user presses enter key.
  "      \"/bin/sh -c \'$remote_dmt_rstr_cmd --host $coord_host --port $coord_port\\\n"
  "      $maybebatch --join --interval \"$checkpoint_interval\"\\\n"
  "      $new_ckpt_files_group\'\" &\n"
  "  fi\n\n"
  "done\n\n"
  "if [ -n \"$localhost_ckpt_files_group\" ]; then\n"
  "exec $dmt_rstr_cmd --host \"$coord_host\" --port \"$coord_port\" $maybebatch\\\n"
  "  $maybejoin --interval \"$checkpoint_interval\" $localhost_ckpt_files_group\n"
  "fi\n\n"

  "#wait for them all to finish\n"
  "wait\n"
;

static bool exitOnLast = false;
static bool blockUntilDone = false;
static int blockUntilDoneRemote = -1;

static dmtcp::DmtcpCoordinator prog;

/* The coordinator can receive a second checkpoint request while processing the
 * first one.  If the second request comes at a point where the coordinator has
 * broadcasted DMTCP_DO_SUSPEND message but the workers haven't replied, the
 * coordinator sends another DMTCP_DO_SUSPEND message.  The workers having
 * replied to the first DMTCP_DO_SUSPEND message (by suspending all the user
 * threads) are waiting for the next message (DMT_DO_FD_LEADER_ELECTION or
 * DMT_KILL_PEER), however they receive DMT_DO_SUSPEND message and thus exit()
 * indicating an error.
 * The fix to this problem is to introduce a global
 * variable "workersRunningAndSuspendMsgSent" which, as the name implies,
 * indicates that the DMT_DO_SUSPEND message has been sent and the coordinator
 * is waiting for replies from the workers.  If this variable is set, the
 * coordinator will not process another checkpoint request.
*/
static bool workersRunningAndSuspendMsgSent = false;

static bool killInProgress = false;

/* If dmtcp_checkpoint/dmtcp_restart specifies '-i', theCheckpointInterval
 * will be reset accordingly (valid for current computation).  If dmtcp_command
 * specifies '-i' (or if user interactively invokes 'i' in coordinator),
 * then both theCheckpointInterval and theDefaultCheckpointInterval are set.
 * A value of '0' means:  never checkpoint (manual checkpoint only).
 */
static int theCheckpointInterval = 0; /* Current checkpoint interval */
static int theDefaultCheckpointInterval = 0; /* Reset to this on new comp. */
static bool batchMode = false;
static bool isRestarting = false;

const int STDIN_FD = fileno ( stdin );

JTIMER ( checkpoint );
JTIMER ( restart );

static int numPeers = -1;
static time_t curTimeStamp = -1;

static dmtcp::LookupService lookupService;
static dmtcp::string localHostName;
static dmtcp::string localPrefix;
static dmtcp::string remotePrefix;

#define INITIAL_VIRTUAL_PID 40000
#define MAX_VIRTUAL_PID   4000000
static pid_t _nextVirtualPid = INITIAL_VIRTUAL_PID;

namespace
{
  static int theNextClientNumber = 1;

  class NamedChunkReader : public jalib::JChunkReader
  {
    public:
      NamedChunkReader ( const jalib::JSocket& sock
                         ,const struct sockaddr * remote
                         ,socklen_t len
                         ,dmtcp::DmtcpMessage &hello_remote)
          : jalib::JChunkReader ( sock, sizeof ( dmtcp::DmtcpMessage ) )
          , _clientNumber ( theNextClientNumber++ )
      {
        _identity = hello_remote.from;
        _state = hello_remote.state;
      }
      const dmtcp::UniquePid& identity() const { return _identity;}
      void identity(dmtcp::UniquePid upid) { _identity = upid;}
      int clientNumber() const { return _clientNumber; }
      dmtcp::WorkerState state() const { return _state; }
      void setState ( dmtcp::WorkerState value ) { _state = value; }
      void progname(dmtcp::string pname){ _progname = pname; }
      dmtcp::string progname(void) const { return _progname; }
      void hostname(dmtcp::string hname){ _hostname = hname; }
      dmtcp::string hostname(void) const { return _hostname; }
      dmtcp::string prefixDir(void) const { return _prefixDir; }
      pid_t virtualPid(void) const { return _virtualPid; }
      void virtualPid(pid_t pid) { _virtualPid = pid; }

      void readProcessInfo(dmtcp::DmtcpMessage& msg) {
        if (msg.extraBytes > 0) {
          char* extraData = new char[msg.extraBytes];
          _sock.readAll(extraData, msg.extraBytes);
          _hostname = extraData;
          _progname = extraData + _hostname.length() + 1;
          if (msg.extraBytes > _hostname.length() + _progname.length() + 2) {
            _prefixDir = extraData + _hostname.length() + _progname.length() + 2;
          }
          delete [] extraData;
        }
      }

    private:
      dmtcp::UniquePid _identity;
      int _clientNumber;
      dmtcp::WorkerState _state;
      dmtcp::string _hostname;
      dmtcp::string _progname;
      dmtcp::string _prefixDir;
      pid_t         _virtualPid;
  };
}

pid_t dmtcp::DmtcpCoordinator::getNewVirtualPid()
{
  pid_t pid = -1;
  JASSERT(_virtualPidToChunkReaderMap.size() < MAX_VIRTUAL_PID/100)
    .Text("Exceeded maximum number of processes allowed");
  while (1) {
    pid = _nextVirtualPid;
    _nextVirtualPid += 1000;
    if (_nextVirtualPid > MAX_VIRTUAL_PID) {
      _nextVirtualPid = INITIAL_VIRTUAL_PID;
    }
    if (_virtualPidToChunkReaderMap.find(pid)
          == _virtualPidToChunkReaderMap.end()) {
      break;
    }
  }
  JASSERT(pid != -1) .Text("Not Reachable");
  return pid;
}

void dmtcp::DmtcpCoordinator::handleUserCommand(char cmd, DmtcpMessage* reply /*= NULL*/)
{
  if (reply != NULL) reply->coordErrorCode = CoordinatorAPI::NOERROR;

  switch ( cmd ){
  case 'b': case 'B':  // prefix blocking command, prior to checkpoint command
    JTRACE ( "blocking checkpoint beginning..." );
    blockUntilDone = true;
    break;
  case 'c': case 'C':
    JTRACE ( "checkpointing..." );
    if(startCheckpoint()){
      if (reply != NULL) reply->numPeers = getStatus().numPeers;
    }else{
      if (reply != NULL) reply->coordErrorCode = CoordinatorAPI::ERROR_NOT_RUNNING_STATE;
    }
    break;
  case 'i': case 'I':
    JTRACE("setting timeout interval...");
    setTimeoutInterval ( theCheckpointInterval );
    if (theCheckpointInterval == 0)
      printf("Current Checkpoint Interval:"
             " Disabled (checkpoint manually instead)\n");
    else
      printf("Current Checkpoint Interval: %d\n", theCheckpointInterval);
    if (theDefaultCheckpointInterval == 0)
      printf("Default Checkpoint Interval:"
             " Disabled (checkpoint manually instead)\n");
    else
      printf("Default Checkpoint Interval: %d\n", theDefaultCheckpointInterval);
    break;
  case 'l': case 'L':
  case 't': case 'T':
    JASSERT_STDERR << "Client List:\n";
    JASSERT_STDERR << "#, PROG[PID]@HOST, DMTCP-UNIQUEPID, STATE\n";
    for ( dmtcp::vector<jalib::JReaderInterface*>::iterator i = _dataSockets.begin()
            ;i!= _dataSockets.end()
            ;++i )
    {
      if ( ( *i )->socket().sockfd() != STDIN_FD )
      {
        const NamedChunkReader& cli = *((NamedChunkReader*)(*i));
        JASSERT_STDERR << cli.clientNumber()
                       << ", " << cli.progname() << "["  << cli.identity().pid() << "]@"  << cli.hostname()
                       << ", " << cli.identity()
                       << ", " << cli.state().toString()
                       << '\n';
      }
    }
    break;
  case 'f': case 'F':
    JNOTE ( "forcing restart..." );
    broadcastMessage ( DMT_FORCE_RESTART );
    break;
  case 'q': case 'Q':
  {
    JNOTE ( "killing all connected peers and quitting ..." );
    broadcastMessage ( DMT_KILL_PEER );
    /* Call to broadcastMessage only puts the messages into the write queue.
     * We actually want the messages to be written out to the respective sockets
     * so that we can then close the sockets and exit gracefully.  The following
     * loop is taken from the implementation of monitorSocket() implementation
     * in jsocket.cpp.
     *
     * Once the messages have been written out, the coordinator closes all the
     * connections and calls exit().
     */
    for ( size_t i=0; i<_writes.size(); ++i )
    {
      int fd = _writes[i]->socket().sockfd();
      if ( fd >= 0 ) {
        _writes[i]->writeOnce();
      }
    }
    JASSERT_STDERR << "DMTCP coordinator exiting... (per request)\n";
    for ( dmtcp::vector<jalib::JReaderInterface*>::iterator i = _dataSockets.begin()
        ; i!= _dataSockets.end()
        ; ++i )
    {
      (*i)->socket().close();
    }
    for ( dmtcp::vector<jalib::JSocket>::iterator i = _listenSockets.begin()
        ; i!= _listenSockets.end()
        ; ++i )
    {
      i->close();
    }
    JTRACE ("Exiting ...");
    exit ( 0 );
    break;
  }
  case 'k': case 'K':
    JNOTE ( "Killing all connected Peers..." );
    //FIXME: What happens if a 'k' command is followed by a 'c' command before
    //       the *real* broadcast takes place?         --Kapil
    broadcastMessage ( DMT_KILL_PEER );
    break;
  case 'h': case 'H': case '?':
    JASSERT_STDERR << theHelpMessage;
    break;
  case 's': case 'S':
    {
      CoordinatorStatus s = getStatus();
      bool running = s.minimumStateUnanimous &&
		     s.minimumState==WorkerState::RUNNING;
      if (reply == NULL){
        printf("Status...\n");
        printf("NUM_PEERS=%d\n", s.numPeers);
        printf("RUNNING=%s\n", (running?"yes":"no"));
        fflush(stdout);
        if (!running) {
          JTRACE("raw status")(s.minimumState)(s.minimumStateUnanimous);
        }
      } else {
        reply->numPeers = s.numPeers;
        reply->isRunning = running;
      }
    }
    break;
  case ' ': case '\t': case '\n': case '\r':
    //ignore whitespace
    break;
  default:
    JTRACE("unhandled user command")(cmd);
    if (reply != NULL){
      reply->coordErrorCode = CoordinatorAPI::ERROR_INVALID_COMMAND;
    }
  }
  return;
}

void dmtcp::DmtcpCoordinator::updateMinimumState(dmtcp::WorkerState oldState)
{
  WorkerState newState = minimumState();

  if ( oldState == WorkerState::RUNNING
       && newState == WorkerState::SUSPENDED )
  {
    JNOTE ( "locking all nodes" );
    broadcastMessage(DMT_DO_FD_LEADER_ELECTION,
                     UniquePid::ComputationId(),
                     getStatus().numPeers );
  }
  if ( oldState == WorkerState::SUSPENDED
       && newState == WorkerState::FD_LEADER_ELECTION )
  {
    JNOTE ( "draining all nodes" );
    broadcastMessage ( DMT_DO_DRAIN );
  }
  if ( oldState == WorkerState::FD_LEADER_ELECTION
       && newState == WorkerState::DRAINED )
  {
    JNOTE ( "checkpointing all nodes" );
    broadcastMessage ( DMT_DO_CHECKPOINT );
  }

#ifdef COORD_NAMESERVICE
  if ( oldState == WorkerState::DRAINED
       && newState == WorkerState::CHECKPOINTED )
  {
    writeRestartScript();
    JNOTE ( "building name service database" );
    lookupService.reset();
    broadcastMessage ( DMT_DO_REGISTER_NAME_SERVICE_DATA );
  }
  if ( oldState == WorkerState::RESTARTING
       && newState == WorkerState::CHECKPOINTED )
  {
    JTIMER_STOP ( restart );

    lookupService.reset();
    JNOTE ( "building name service database (after restart)" );
    broadcastMessage ( DMT_DO_REGISTER_NAME_SERVICE_DATA );
  }
  if ( oldState == WorkerState::CHECKPOINTED
       && newState == WorkerState::NAME_SERVICE_DATA_REGISTERED ){
    JNOTE ( "entertaining queries now" );
    broadcastMessage ( DMT_DO_SEND_QUERIES );
  }
  if ( oldState == WorkerState::NAME_SERVICE_DATA_REGISTERED
       && newState == WorkerState::DONE_QUERYING ){
    JNOTE ( "refilling all nodes" );
    broadcastMessage ( DMT_DO_REFILL );
  }
  if ( oldState == WorkerState::DONE_QUERYING
       && newState == WorkerState::REFILLED )
#else
    if ( oldState == WorkerState::DRAINED
         && newState == WorkerState::CHECKPOINTED )
    {
      JNOTE ( "refilling all nodes" );
      broadcastMessage ( DMT_DO_REFILL );
      writeRestartScript();
    }
  if ( oldState == WorkerState::RESTARTING
       && newState == WorkerState::CHECKPOINTED )
  {
    JTIMER_STOP ( restart );

    JNOTE ( "refilling all nodes (after checkpoint)" );
    broadcastMessage ( DMT_DO_REFILL );
  }
  if ( oldState == WorkerState::CHECKPOINTED
       && newState == WorkerState::REFILLED )
#endif
  {
    JNOTE ( "restarting all nodes" );
    broadcastMessage ( DMT_DO_RESUME );

    JTIMER_STOP ( checkpoint );
    isRestarting = false;

    setTimeoutInterval( theCheckpointInterval );

    if (blockUntilDone) {
      DmtcpMessage blockUntilDoneReply(DMT_USER_CMD_RESULT);
      JNOTE ( "replying to dmtcp_command:  we're done" );
      // These were set in dmtcp::DmtcpCoordinator::onConnect in this file
      jalib::JSocket remote ( blockUntilDoneRemote );
      remote << blockUntilDoneReply;
      remote.close();
      blockUntilDone = false;
      blockUntilDoneRemote = -1;
    }
  }
}

void dmtcp::DmtcpCoordinator::onData ( jalib::JReaderInterface* sock )
{
  if ( sock->socket().sockfd() == STDIN_FD )
  {
    handleUserCommand(sock->buffer()[0]);
    return;
  }
  else
  {
    NamedChunkReader * client= ( NamedChunkReader* ) sock;
    DmtcpMessage& msg = * ( DmtcpMessage* ) sock->buffer();
    msg.assertValid();
    char * extraData = 0;

    if ( msg.extraBytes > 0 )
    {
      extraData = new char[msg.extraBytes];
      sock->socket().readAll ( extraData, msg.extraBytes );
    }

    switch ( msg.type )
    {
      case DMT_OK:
      {
        WorkerState oldState = client->state();
        client->setState ( msg.state );
        CoordinatorStatus s = getStatus();
        WorkerState newState = s.minimumState;
        /* It is possible for minimumState to be RUNNING while one or more
         * processes are still in REFILLED state.
         */
        if (s.minimumState == WorkerState::RUNNING && !s.minimumStateUnanimous &&
            s.maximumState == WorkerState::REFILLED) {
          /* If minimumState is RUNNING, and not every processes is in RUNNING
           * state, the maximumState must be REFILLED. This is the case when we
           * are performing ckpt-resume or rst-resume).
           */
          newState = s.maximumState;
        }

        JTRACE ("got DMT_OK message")
          ( msg.from )( msg.state )( oldState )( newState );

        updateMinimumState(oldState);
        break;
      }
      case DMT_CKPT_FILENAME:
      {
        JASSERT ( extraData!=0 )
          .Text ( "extra data expected with DMT_CKPT_FILENAME message" );
        dmtcp::string ckptFilename;
        dmtcp::string hostname;
        ckptFilename = extraData;
        hostname = extraData + ckptFilename.length() + 1;

        JTRACE ( "recording restart info" ) ( ckptFilename ) ( hostname );
        _restartFilenames[hostname].push_back ( ckptFilename );
      }
      break;
      case DMT_USER_CMD:  // dmtcpaware API being used
        {
          JTRACE("got user command from client")
            (msg.coordCmd)(client->identity());
	  // Checkpointing commands should always block, to prevent
	  //   dmtcpaware checkpoint call from returning prior to checkpoint.
	  if (msg.coordCmd == 'c')
            handleUserCommand( 'b', NULL );
          DmtcpMessage reply;
          reply.type = DMT_USER_CMD_RESULT;
          if (msg.coordCmd == 'i' &&  msg.theCheckpointInterval > 0 ) {
            theCheckpointInterval = msg.theCheckpointInterval;
            // For dmtcpaware API, we don't change theDefaultCheckpointInterval
          }
          handleUserCommand( msg.coordCmd, &reply );
          sock->socket() << reply;
          //alternately, we could do the write without blocking:
          //addWrite(new jalib::JChunkWriter(sock->socket(), (char*)&msg,
          //                                 sizeof(DmtcpMessage)));
        }
        break;


#ifdef COORD_NAMESERVICE
      case DMT_REGISTER_NAME_SERVICE_DATA:
      {
        JTRACE ("received REGISTER_NAME_SERVICE_DATA msg") (client->identity());
        lookupService.registerData(client->identity(), msg,
                                   (const char*) extraData);
      }
      break;
      case DMT_NAME_SERVICE_QUERY:
      {
        JTRACE ("received NAME_SERVICE_QUERY msg") (client->identity());
        lookupService.respondToQuery(client->identity(), sock->socket(), msg,
                                     (const char*) extraData);
      }
      break;
#endif
      case DMT_UPDATE_PROCESS_INFO_AFTER_FORK:
      {
          dmtcp::string hostname = extraData;
          dmtcp::string progname = extraData + hostname.length() + 1;
          JNOTE("Updating process Information after fork()")
            (hostname) (progname) (msg.from) (client->identity());
          client->progname(progname);
          client->hostname(hostname);
          client->identity(msg.from);
      }
          break;
      default:
        JASSERT ( false ) ( msg.from ) ( msg.type )
		.Text ( "unexpected message from worker" );
    }

    delete[] extraData;
  }
}

void dmtcp::DmtcpCoordinator::onDisconnect ( jalib::JReaderInterface* sock )
{
  if ( sock->socket().sockfd() == STDIN_FD ) {
    JTRACE ( "stdin closed" );
  } else {
    NamedChunkReader& client = * ( ( NamedChunkReader* ) sock );
    JNOTE ( "client disconnected" ) ( client.identity() );
    _virtualPidToChunkReaderMap.erase(client.virtualPid());

    CoordinatorStatus s = getStatus();
    if (s.numPeers < 1) {
      if (exitOnLast) {
        JNOTE ("last client exited, shutting down..");
        handleUserCommand('q');
      }
      // If a kill in is progress, the coordinator refuses any new connections,
      // thus we need to reset it to false once all the processes in the
      // computations have disconnected.
      killInProgress = false;
      if (theCheckpointInterval != theDefaultCheckpointInterval) {
        theCheckpointInterval = theDefaultCheckpointInterval;
        JNOTE ( "CheckpointInterval reset on end of current computation" )
	  ( theCheckpointInterval );
      }
    } else {
      updateMinimumState(client.state());
    }
  }
}

void dmtcp::DmtcpCoordinator::initializeComputation()
{
  //this is the first connection, do some initializations
  workersRunningAndSuspendMsgSent = false;
  killInProgress = false;
  //_nextVirtualPid = INITIAL_VIRTUAL_PID;

  theCheckpointInterval = theDefaultCheckpointInterval;
  setTimeoutInterval( theCheckpointInterval );
  // theCheckpointInterval can be overridden later by msg from this client.

  // drop current computation group to 0
  UniquePid::ComputationId() = dmtcp::UniquePid(0,0,0);
  curTimeStamp = 0; // Drop timestamp to 0
  numPeers = -1; // Drop number of peers to unknown
}

void dmtcp::DmtcpCoordinator::onConnect ( const jalib::JSocket& sock,
                                          const struct sockaddr* remoteAddr,
                                          socklen_t remoteLen )
{
  jalib::JSocket remote ( sock );
  // If no client is connected to Coordinator, then there can be only zero data
  // sockets OR there can be one data socket and that should be STDIN.
  if ( _dataSockets.size() == 0 ||
       ( _dataSockets.size() == 1
	 && _dataSockets[0]->socket().sockfd() == STDIN_FD ) ) {
    initializeComputation();
  }

  dmtcp::DmtcpMessage hello_remote;
  hello_remote.poison();
  JTRACE("Reading from incoming connection...");
  remote >> hello_remote;
  if (!remote.isValid()) {
    remote.close();
    return;
  }

  if (hello_remote.type == DMT_GET_VIRTUAL_PID) {
    dmtcp::DmtcpMessage reply(DMT_GET_VIRTUAL_PID_RESULT);
    reply.virtualPid = getNewVirtualPid();
    JASSERT(reply.virtualPid != -1);
    remote << reply;
    return;
  }

  if (hello_remote.type == DMT_USER_CMD) {
    processDmtUserCmd(hello_remote, remote);
    return;
  }

  if (killInProgress) {
    JNOTE("Connection request received in the middle of killing computation. "
          "Sending it the kill message.");
    DmtcpMessage msg;
    msg.type = DMT_KILL_PEER;
    remote << msg;
    remote.close();
    return;
  }

  NamedChunkReader *ds = new NamedChunkReader(sock, remoteAddr, remoteLen,
                                              hello_remote);

  if (hello_remote.virtualPid == -1) {
    ds->virtualPid(getNewVirtualPid());
  } else {
    ds->virtualPid(hello_remote.virtualPid);
  }

  if( hello_remote.extraBytes > 0 ){
    ds->readProcessInfo(hello_remote);
  }

  if ( hello_remote.type == DMT_RESTART_PROCESS ) {
    if ( validateDmtRestartProcess ( hello_remote, remote ) == false )
      return;
    isRestarting = true;
  } else if ( hello_remote.type == DMT_HELLO_COORDINATOR &&
              hello_remote.state == WorkerState::RESTARTING) {
    if ( validateRestartingWorkerProcess ( hello_remote, remote ) == false )
      return;
    //JASSERT(hello_remote.virtualPid != -1);
    ds->virtualPid(hello_remote.virtualPid);
    _virtualPidToChunkReaderMap[ds->virtualPid()] = ds;
    isRestarting = true;
  } else if ( hello_remote.type == DMT_HELLO_COORDINATOR &&
              (hello_remote.state == WorkerState::RUNNING ||
               hello_remote.state == WorkerState::UNKNOWN)) {
    if ( validateNewWorkerProcess ( hello_remote, remote, ds ) == false )
      return;
    _virtualPidToChunkReaderMap[ds->virtualPid()] = ds;
  } else {
    JASSERT ( false )
      .Text ( "Connect request from Unknown Remote Process Type" );
  }

  JNOTE ( "worker connected" ) ( hello_remote.from );

  if ( hello_remote.theCheckpointInterval != DMTCPMESSAGE_SAME_CKPT_INTERVAL ) {
    int oldInterval = theCheckpointInterval;
    theCheckpointInterval = hello_remote.theCheckpointInterval;
    setTimeoutInterval ( theCheckpointInterval );
    JNOTE ( "CheckpointInterval updated (for this computation only)" )
	  ( oldInterval ) ( theCheckpointInterval );
  }

  //add this client as a chunk reader
  // in this case a 'chunk' is sizeof(DmtcpMessage)
  addDataSocket ( ds );

  JTRACE( "END" )
  ( _dataSockets.size() ) ( _dataSockets[0]->socket().sockfd() == STDIN_FD );
}

void dmtcp::DmtcpCoordinator::processDmtUserCmd( DmtcpMessage& hello_remote,
						 jalib::JSocket& remote )
{
  //dmtcp_command doesn't handshake (it is antisocial)
  JTRACE("got user command from dmtcp_command")(hello_remote.coordCmd);
  DmtcpMessage reply;
  reply.type = DMT_USER_CMD_RESULT;
  // if previous 'b' blocking prefix command had set blockUntilDone
  if (blockUntilDone && blockUntilDoneRemote == -1  &&
      hello_remote.coordCmd == 'c') {
    // Reply will be done in dmtcp::DmtcpCoordinator::onData in this file.
    blockUntilDoneRemote = remote.sockfd();
    handleUserCommand( hello_remote.coordCmd, &reply );
  } else if ( (hello_remote.coordCmd == 'i')
               && hello_remote.theCheckpointInterval >= 0 ) {
    theDefaultCheckpointInterval = hello_remote.theCheckpointInterval;
    theCheckpointInterval = theDefaultCheckpointInterval;
    handleUserCommand( hello_remote.coordCmd, &reply );
    remote << reply;
    remote.close();
  } else {
    handleUserCommand( hello_remote.coordCmd, &reply );
    remote << reply;
    remote.close();
  }
  return;
}

bool dmtcp::DmtcpCoordinator::validateDmtRestartProcess
	 ( DmtcpMessage& hello_remote, jalib::JSocket& remote )
{
  struct timeval tv;
  // This is dmtcp_restart process, connecting to get timestamp
  // and set current compGroup.

  JASSERT ( hello_remote.numPeers > 0 );

  dmtcp::DmtcpMessage hello_local ( dmtcp::DMT_RESTART_PROCESS_REPLY );

  if( UniquePid::ComputationId() == dmtcp::UniquePid(0,0,0) ){
    JASSERT ( minimumState() == WorkerState::UNKNOWN )
      .Text ( "Coordinator should be idle at this moment" );
    // Coordinator is free at this moment - set up all the things
    UniquePid::ComputationId() = hello_remote.compGroup;
    numPeers = hello_remote.numPeers;
    JASSERT(gettimeofday(&tv, NULL) == 0);
    // Get the resolution down to 100 mili seconds.
    curTimeStamp = (tv.tv_sec << 4) | (tv.tv_usec / (100*1000));
    JNOTE ( "FIRST dmtcp_restart connection.  Set numPeers. Generate timestamp" )
      ( numPeers ) ( curTimeStamp ) ( UniquePid::ComputationId() );
    JTIMER_START(restart);
  } else if ( UniquePid::ComputationId() != hello_remote.compGroup ) {
    // Coordinator already serving some other computation group - reject this process.
    JNOTE ("Reject incoming dmtcp_restart connection"
           " since it is not from current computation")
      ( UniquePid::ComputationId() ) ( hello_remote.compGroup );
    hello_local.type = dmtcp::DMT_REJECT;
    remote << hello_local;
    remote.close();
    return false;
  } else if ( numPeers != hello_remote.numPeers ) {
    // Sanity check
    JNOTE  ( "Invalid numPeers reported by dmtcp_restart process, Rejecting" )
      ( numPeers ) ( hello_remote.numPeers );

    hello_local.type = dmtcp::DMT_REJECT;
    remote << hello_local;
    remote.close();
    return false;
  } else {
    // This is a second or higher dmtcp_restart process connecting to the coordinator.
    // FIXME: Should the following be a JASSERT instead?      -- Kapil
    JWARNING ( minimumState() == WorkerState::RESTARTING );
  }

  // Sent generated timestamp in local massage for dmtcp_restart process.
  hello_local.coordTimeStamp = curTimeStamp;

  remote << hello_local;

  return true;
}

bool dmtcp::DmtcpCoordinator::validateRestartingWorkerProcess
	 ( DmtcpMessage& hello_remote, jalib::JSocket& remote )
{
  struct timeval tv;
  dmtcp::DmtcpMessage hello_local ( dmtcp::DMT_HELLO_WORKER );

  JASSERT(hello_remote.state == WorkerState::RESTARTING) (hello_remote.state);

  if (UniquePid::ComputationId() == dmtcp::UniquePid(0,0,0)) {
    JASSERT ( minimumState() == WorkerState::UNKNOWN )
      .Text ( "Coordinator should be idle at this moment" );
    // Coordinator is free at this moment - set up all the things
    UniquePid::ComputationId() = hello_remote.compGroup;
    numPeers = hello_remote.numPeers;
    JASSERT(gettimeofday(&tv, NULL) == 0);
    // Get the resolution down to 100 mili seconds.
    curTimeStamp = (tv.tv_sec << 4) | (tv.tv_usec / (100*1000));
    JNOTE ( "FIRST dmtcp_restart connection.  Set numPeers. Generate timestamp" )
      ( numPeers ) ( curTimeStamp ) ( UniquePid::ComputationId() );
    JTIMER_START(restart);
  } else if (minimumState() != WorkerState::RESTARTING &&
             minimumState() != WorkerState::CHECKPOINTED) {
    JNOTE ("Computation not in RESTARTING or CHECKPOINTED state."
           "  Reject incoming restarting computation process.")
      (UniquePid::ComputationId()) (hello_remote.compGroup) (minimumState());
    hello_local.type = dmtcp::DMT_REJECT;
    remote << hello_local;
    remote.close();
    return false;
  } else if ( hello_remote.compGroup != UniquePid::ComputationId()) {
    JNOTE ("Reject incoming restarting computation process"
           " since it is not from current computation")
      ( UniquePid::ComputationId() ) ( hello_remote.compGroup );
    hello_local.type = dmtcp::DMT_REJECT;
    remote << hello_local;
    remote.close();
    return false;
  }
  // dmtcp_restart already connected and compGroup created.
  // Computation process connection
  JASSERT ( curTimeStamp != 0 );

  JTRACE("Connection from (restarting) computation process")
    ( UniquePid::ComputationId() ) ( hello_remote.compGroup ) ( minimumState() );

  hello_local.coordTimeStamp = curTimeStamp;
  remote << hello_local;

  // NOTE: Sending the same message twice. We want to make sure that the
  // worker process receives/processes the first messages as soon as it
  // connects to the coordinator. The second message will be processed in
  // postRestart routine in DmtcpWorker.
  //
  // The reason to do this is the following. The dmtcp_restart process
  // connects to the coordinator at a very early stage. Later on, before
  // exec()'ing into mtcp_restart, it reconnects to the coordinator using
  // it's original UniquiePid and closes the earlier socket connection.
  // However, the coordinator might process the disconnect() before it
  // processes the connect() which would lead to a situation where the
  // coordinator is not connected to any worker processes. The coordinator
  // would now process the connect() and may reject the worker because the
  // worker state is RESTARTING, but the minimumState() is UNKNOWN.
  //remote << hello_local;

  return true;
}

bool dmtcp::DmtcpCoordinator::validateNewWorkerProcess
  (DmtcpMessage& hello_remote, jalib::JSocket& remote, jalib::JChunkReader *jcr)
{
  NamedChunkReader *ds = (NamedChunkReader*) jcr;
  dmtcp::DmtcpMessage hello_local(dmtcp::DMT_HELLO_WORKER);
  hello_local.virtualPid = ds->virtualPid();
  CoordinatorStatus s = getStatus();

  JASSERT(hello_remote.state == WorkerState::RUNNING ||
          hello_remote.state == WorkerState::UNKNOWN) (hello_remote.state);

  if (workersRunningAndSuspendMsgSent == true) {
    /* Worker trying to connect after SUSPEND message has been sent.
     * This happens if the worker process is executing a fork() system call
     * when the DMT_DO_SUSPEND is broadcasted. We need to make sure that the
     * child process is allowed to participate in the current checkpoint.
     */
    JASSERT(s.numPeers > 0) (s.numPeers);
    JASSERT(s.minimumState != WorkerState::SUSPENDED) (s.minimumState);

    // Handshake
    hello_local.compGroup = UniquePid::ComputationId();
    remote << hello_local;

    // Now send DMT_DO_SUSPEND message so that this process can also
    // participate in the current checkpoint
    DmtcpMessage suspendMsg (dmtcp::DMT_DO_SUSPEND);
    remote << suspendMsg;

  } else if (s.numPeers > 0 && s.minimumState != WorkerState::RUNNING &&
             s.minimumState != WorkerState::UNKNOWN) {
    // If some of the processes are not in RUNNING state
    JNOTE("Current computation not in RUNNING state."
          "  Refusing to accept new connections.")
      (UniquePid::ComputationId()) (hello_remote.from)
      (s.numPeers) (s.minimumState);
    hello_local.type = dmtcp::DMT_REJECT;
    remote << hello_local;
    remote.close();
    return false;

  } else if (hello_remote.compGroup != UniquePid()) {
    // New Process trying to connect to Coordinator but already has compGroup
    JNOTE  ( "New process, but already has computation group,\n"
             "OR perhaps a different DMTCP_PREFIX_ID.  Rejecting." )
      (hello_remote.compGroup);

    hello_local.type = dmtcp::DMT_REJECT;
    remote << hello_local;
    remote.close();
    return false;

  } else {
    // If first process, create the new computation group
    if (UniquePid::ComputationId() == UniquePid(0,0,0)) {
      struct timeval tv;
      // Connection of new computation.
      UniquePid::ComputationId() = hello_remote.from;
      localPrefix.clear();
      localHostName.clear();
      remotePrefix.clear();
      if (!ds->prefixDir().empty()) {
        localPrefix = ds->prefixDir();
        localHostName = ds->hostname();
      }
      JASSERT(gettimeofday(&tv, NULL) == 0);
      // Get the resolution down to 100 mili seconds.
      curTimeStamp = (tv.tv_sec << 4) | (tv.tv_usec / (100*1000));
      numPeers = -1;
      JTRACE("First process connected.  Creating new computation group")
        (UniquePid::ComputationId());
    } else {
      JTRACE("New process connected")
        (hello_remote.from) (ds->prefixDir()) (ds->virtualPid());
      if (ds->hostname() == localHostName) {
        JASSERT(ds->prefixDir() == localPrefix) (ds->prefixDir()) (localPrefix);
      }
      if (!ds->prefixDir().empty() && ds->hostname() != localHostName) {
        if (remotePrefix.empty()) {
          JASSERT (UniquePid::ComputationId() != UniquePid(0,0,0));
          remotePrefix = ds->prefixDir();
        } else if (remotePrefix != ds->prefixDir()) {
          JNOTE("This node has different prefixDir than the rest of the "
                "remote nodes. Rejecting connection!")
            (remotePrefix) (localPrefix) (ds->prefixDir());
          hello_local.type = dmtcp::DMT_REJECT;
          remote << hello_local;
          remote.close();
          return false;
        }
      }
    }
    hello_local.compGroup = UniquePid::ComputationId();
    hello_local.coordTimeStamp = curTimeStamp;
    remote << hello_local;
  }
  return true;
}

void dmtcp::DmtcpCoordinator::onTimeoutInterval()
{
  if ( theCheckpointInterval > 0 )
    startCheckpoint();
}


bool dmtcp::DmtcpCoordinator::startCheckpoint()
{
  CoordinatorStatus s = getStatus();
  if ( s.minimumState == WorkerState::RUNNING && s.minimumStateUnanimous
       && !workersRunningAndSuspendMsgSent )
  {
    JTIMER_START ( checkpoint );
    _restartFilenames.clear();
    JNOTE ( "starting checkpoint, suspending all nodes" )( s.numPeers );
    UniquePid::ComputationId().incrementGeneration();
    JNOTE("Incremented Generation") (UniquePid::ComputationId().generation());
    // Pass number of connected peers to all clients
    broadcastMessage(DMT_DO_SUSPEND);

    // Suspend Message has been sent but the workers are still in running
    // state.  If the coordinator receives another checkpoint request from user
    // at this point, it should fail.
    workersRunningAndSuspendMsgSent = true;
    return true;
  } else {
    if (s.numPeers > 0) {
      JTRACE ( "delaying checkpoint, workers not ready" ) ( s.minimumState )
	     ( s.numPeers );
    }
    return false;
  }
}

void dmtcp::DmtcpCoordinator::broadcastMessage ( DmtcpMessageType type,
    dmtcp::UniquePid compGroup = dmtcp::UniquePid(), int numPeers = -1 )
{
  DmtcpMessage msg;
  msg.type = type;
  if (numPeers > 0) {
    msg.numPeers = numPeers;
    msg.compGroup = compGroup;
  }

  broadcastMessage ( msg );
  JTRACE ("sending message")( type );
}

void dmtcp::DmtcpCoordinator::broadcastMessage ( const DmtcpMessage& msg )
{
  if (msg.type == DMT_KILL_PEER) {
    killInProgress = true;
  } else if (msg.type == DMT_DO_FD_LEADER_ELECTION) {
    // All the workers are in SUSPENDED state, now it is safe to reset
    // this flag.
    workersRunningAndSuspendMsgSent = false;
  }

  for ( dmtcp::vector<jalib::JReaderInterface*>::iterator i
	= _dataSockets.begin() ; i!= _dataSockets.end() ; i++ )
  {
    if ( ( *i )->socket().sockfd() != STDIN_FD )
      addWrite ( new jalib::JChunkWriter ( ( *i )->socket(),
					   ( char* ) &msg,
					   sizeof ( DmtcpMessage ) ) );
  }
}

dmtcp::DmtcpCoordinator::CoordinatorStatus dmtcp::DmtcpCoordinator::getStatus() const
{
  CoordinatorStatus status;
  const static int INITIAL_MIN = WorkerState::_MAX;
  const static int INITIAL_MAX = WorkerState::UNKNOWN;
  int min = INITIAL_MIN;
  int max = INITIAL_MAX;
  int count = 0;
  bool unanimous = true;
  for ( const_iterator i = _dataSockets.begin()
      ; i != _dataSockets.end()
      ; ++i )
  {
    if ( ( *i )->socket().sockfd() != STDIN_FD )
    {
      int cliState = ((NamedChunkReader*)*i)->state().value();
      count++;
      unanimous = unanimous && (min==cliState || min==INITIAL_MIN);
      if ( cliState < min ) min = cliState;
      if ( cliState > max ) max = cliState;
    }
  }

  status.minimumState = ( min==INITIAL_MIN ? WorkerState::UNKNOWN
			  : (WorkerState::eWorkerState)min );
  if( status.minimumState == WorkerState::CHECKPOINTED &&
      isRestarting && count < numPeers ){
    JTRACE("minimal state counted as CHECKPOINTED but not all processes"
	   " are connected yet.  So we wait.") ( numPeers ) ( count );
    status.minimumState = WorkerState::RESTARTING;
  }
  status.minimumStateUnanimous = unanimous;

  status.maximumState = ( max==INITIAL_MAX ? WorkerState::UNKNOWN
			  : (WorkerState::eWorkerState)max );
  status.numPeers = count;
  return status;
}

void dmtcp::DmtcpCoordinator::writeRestartScript()
{
  const char* dir = getenv ( ENV_VAR_CHECKPOINT_DIR );
  if(dir==NULL) dir = ".";
  dmtcp::ostringstream o1, o2;
  dmtcp::string filename, uniqueFilename;

  o1 << dmtcp::string(dir) << "/"
     << RESTART_SCRIPT_BASENAME << RESTART_SCRIPT_EXT;
  filename = o1.str();

  o2 << dmtcp::string(dir) << "/"
     << RESTART_SCRIPT_BASENAME << "_" << UniquePid::ComputationId()
#ifdef UNIQUE_CHECKPOINT_FILENAMES
     << "_"
     << std::setw(5) << std::setfill('0') << UniquePid::ComputationId().generation()
#endif
     << RESTART_SCRIPT_EXT;
  uniqueFilename = o2.str();

  const bool isSingleHost = (_restartFilenames.size() == 1);

  dmtcp::map< dmtcp::string, dmtcp::vector<dmtcp::string> >::const_iterator host;
  dmtcp::vector<dmtcp::string>::const_iterator file;

  char hostname[80];
  gethostname ( hostname, 80 );

  JTRACE ( "writing restart script" ) ( uniqueFilename );

  FILE* fp = fopen ( uniqueFilename.c_str(),"w" );
  JASSERT ( fp!=0 )(JASSERT_ERRNO)( uniqueFilename )
    .Text ( "failed to open file" );

  fprintf ( fp, "%s", theRestartScriptHeader );
  fprintf ( fp, "%s", theRestartScriptCheckLocal );
  fprintf ( fp, "%s", theRestartScriptUsage );

  fprintf ( fp, "coord_host=$"ENV_VAR_NAME_HOST"\n"
                "if test -z \"$" ENV_VAR_NAME_HOST "\"; then\n"
                "  coord_host=%s\nfi\n\n"
                "coord_port=$"ENV_VAR_NAME_PORT"\n"
                "if test -z \"$" ENV_VAR_NAME_PORT "\"; then\n"
                "  coord_port=%d\nfi\n\n"
                "checkpoint_interval=$"ENV_VAR_CKPT_INTR"\n"
                "if test -z \"$" ENV_VAR_CKPT_INTR "\"; then\n"
                "  checkpoint_interval=%d\nfi\n\n",
                hostname, thePort, theCheckpointInterval );

  if ( batchMode )
    fprintf ( fp, "maybebatch='--batch'\n\n" );
  else
    fprintf ( fp, "maybebatch=\n\n" );

  fprintf ( fp, "%s", theRestartScriptCmdlineArgHandler );

  fprintf ( fp, "dmt_rstr_cmd=" DMTCP_RESTART_CMD "\n"
                "which " DMTCP_RESTART_CMD " > /dev/null \\\n"
                " || dmt_rstr_cmd=%s/" DMTCP_RESTART_CMD "\n\n",
                jalib::Filesystem::GetProgramDir().c_str());

  fprintf ( fp, "local_prefix=%s\n", localPrefix.c_str() );
  fprintf ( fp, "remote_prefix=%s\n", remotePrefix.c_str() );
  fprintf ( fp, "remote_dmt_rstr_cmd=" DMTCP_RESTART_CMD "\n"
                "if ! test -z \"$remote_prefix\"; then\n"
                "  remote_dmt_rstr_cmd=\"$remote_prefix/bin/" DMTCP_RESTART_CMD "\"\n"
                "fi\n\n" );

  fprintf ( fp, "# Number of hosts in the computation = %zd\n"
                "# Number of processes in the computation = %d\n\n",
                _restartFilenames.size(), getStatus().numPeers );

  if ( isSingleHost ) {
    JTRACE ( "Single HOST" );

    host=_restartFilenames.begin();
    dmtcp::ostringstream o;
    for ( file=host->second.begin(); file!=host->second.end(); ++file ) {
      o << " " << *file;
    }
    fprintf ( fp, "given_ckpt_files=\"%s\"\n\n", o.str().c_str());

    fprintf ( fp, "%s", theRestartScriptSingleHostProcessing );
  }
  else
  {
    fprintf ( fp, "%s",
              "# SYNTAX:\n"
              "#  :: <HOST> :<MODE>: <CHECKPOINT_IMAGE> ...\n"
              "# Host names and filenames must not include \':\'\n"
              "# At most one fg (foreground) mode allowed; it must be last.\n"
              "# \'maybexterm\' and \'maybebg\' are set from <MODE>.\n");

    fprintf ( fp, "%s", "worker_ckpts=\'" );
    for ( host=_restartFilenames.begin(); host!=_restartFilenames.end(); ++host ) {
      fprintf ( fp, "\n :: %s :bg:", host->first.c_str() );
      for ( file=host->second.begin(); file!=host->second.end(); ++file ) {
        fprintf ( fp," %s", file->c_str() );
      }
    }
    fprintf ( fp, "%s", "\n\'\n\n" );

    fprintf( fp,  "# Check for resource manager\n"
                  "discover_rm_path=$(which dmtcp_discover_rm)\n"
                  "if [ -n \"$discover_rm_path\" ]; then\n"
                  "  eval $(dmtcp_discover_rm \"$worker_ckpts\")\n"
                  "  if [ -n \"$new_worker_ckpts\" ]; then\n"
                  "    worker_ckpts=\"$new_worker_ckpts\"\n"
                  "  fi\n"
                  "fi\n\n"
                  "\n\n\n");

    fprintf ( fp, "%s", theRestartScriptMultiHostProcessing );
  }

  fclose ( fp );
  {
    /* Set execute permission for user. */
    struct stat buf;
    stat ( uniqueFilename.c_str(), &buf );
    chmod ( uniqueFilename.c_str(), buf.st_mode | S_IXUSR );
    // Create a symlink from
    //   dmtcp_restart_script.sh -> dmtcp_restart_script_<curCompId>.sh
    unlink ( filename.c_str() );
    JTRACE("linking \"dmtcp_restart_script.sh\" filename to uniqueFilename")
	  (filename)(uniqueFilename);
    // FIXME:  Handle error case of symlink()
    JWARNING( 0 == symlink ( uniqueFilename.c_str(), filename.c_str() ) );
  }
  _restartFilenames.clear();
}

static void SIGINTHandler(int signum)
{
  prog.handleUserCommand('q');
}

static void setupSIGINTHandler()
{
  struct sigaction action;
  action.sa_handler = SIGINTHandler;
  sigemptyset ( &action.sa_mask );
  action.sa_flags = 0;
  sigaction ( SIGINT, &action, NULL );
}

#define shift argc--; argv++

int main ( int argc, char** argv )
{
  initializeJalib();
  dmtcp::DmtcpMessage::setDefaultCoordinator ( dmtcp::UniquePid::ThisProcess() );

  //parse port
  thePort = DEFAULT_PORT;
  const char* portStr = getenv ( ENV_VAR_NAME_PORT );
  if ( portStr != NULL ) thePort = jalib::StringToInt ( portStr );

  bool background = false;

  shift;
  while(argc > 0){
    dmtcp::string s = argv[0];
    if(s=="-h" || s=="--help"){
      fprintf(stderr, theUsage, DEFAULT_PORT);
      return 1;
    } else if ((s=="--version") && argc==1){
      JASSERT_STDERR << DMTCP_VERSION_AND_COPYRIGHT_INFO;
      return 1;
    }else if(s=="--exit-on-last"){
      exitOnLast = true;
      shift;
    }else if(s=="--background"){
      background = true;
      shift;
    }else if(s=="--batch"){
      batchMode = true;
      shift;
    }else if(argc>1 && (s == "-i" || s == "--interval")){
      setenv(ENV_VAR_CKPT_INTR, argv[1], 1);
      shift; shift;
    }else if(argc>1 && (s == "-p" || s == "--port")){
      thePort = jalib::StringToInt( argv[1] );
      shift; shift;
    }else if(argc>1 && (s == "-c" || s == "--ckptdir")){
      setenv(ENV_VAR_CHECKPOINT_DIR, argv[1], 1);
      shift; shift;
    }else if(argc>1 && (s == "-t" || s == "--tmpdir")){
      setenv(ENV_VAR_TMPDIR, argv[1], 1);
      shift; shift;
    }else if(argc == 1){ //last arg can be port
      char *endptr;
      long x = strtol(argv[0], &endptr, 10);
      if ((ssize_t)strlen(argv[0]) != endptr - argv[0]) {
        fprintf(stderr, theUsage, DEFAULT_PORT);
        return 1;
      } else {
        thePort = jalib::StringToInt( argv[0] );
        shift;
      }
      x++, x--; // to suppress unused variable warning
    }else{
      fprintf(stderr, theUsage, DEFAULT_PORT);
      return 1;
    }
  }

  JASSERT ( ! (background && batchMode) )
    .Text ( "--background and --batch can't be specified together");

  dmtcp::UniquePid::setTmpDir(getenv(ENV_VAR_TMPDIR));

  dmtcp::Util::initializeLogFile();

  JTRACE ( "New DMTCP coordinator starting." )
    ( dmtcp::UniquePid::ThisProcess() );

  if ( thePort < 0 )
  {
    fprintf(stderr, theUsage, DEFAULT_PORT);
    return 1;
  }

  jalib::JServerSocket* sock;
  /*Test if the listener socket is already open*/
  if ( fcntl(PROTECTED_COORD_FD, F_GETFD) != -1 ) {
    sock = new jalib::JServerSocket ( PROTECTED_COORD_FD );
    JASSERT ( sock->port() != -1 ) .Text ( "Invalid listener socket" );
    JTRACE ( "Using already created listener socker" ) ( sock->port() );
  } else {

    errno = 0;
    sock = new jalib::JServerSocket ( jalib::JSockAddr::ANY, thePort );
    JASSERT ( sock->isValid() ) ( thePort ) ( JASSERT_ERRNO )
      .Text ( "Failed to create listen socket."
       "\nIf msg is \"Address already in use\", this may be an old coordinator."
       "\nKill default coordinator and try again:  dmtcp_command -q"
       "\nIf that fails, \"pkill -9 dmtcp_coord\","
       " and try again in a minute or so." );
  }

  thePort = sock->port();

  if ( batchMode && getenv ( ENV_VAR_CKPT_INTR ) == NULL ) {
    setenv(ENV_VAR_CKPT_INTR, "3600", 1);
  }
  //parse checkpoint interval
  const char* interval = getenv ( ENV_VAR_CKPT_INTR );
  if ( interval != NULL ) {
    theDefaultCheckpointInterval = jalib::StringToInt ( interval );
    theCheckpointInterval = theDefaultCheckpointInterval;
  }

#if 0
  JASSERT_STDERR <<
    "dmtcp_coordinator starting..." <<
    "\n    Port: " << thePort <<
    "\n    Checkpoint Interval: ";
  if(theCheckpointInterval==0)
    JASSERT_STDERR << "disabled (checkpoint manually instead)";
  else
    JASSERT_STDERR << theCheckpointInterval;
  JASSERT_STDERR  <<
    "\n    Exit on last client: " << exitOnLast << "\n";
#else
    fprintf(stderr, "dmtcp_coordinator starting..."
    "\n    Port: %d"
    "\n    Checkpoint Interval: ", thePort);
  if(theCheckpointInterval==0)
    fprintf(stderr, "disabled (checkpoint manually instead)");
  else
    fprintf(stderr, "%d", theCheckpointInterval);
  fprintf(stderr, "\n    Exit on last client: %d\n", exitOnLast);
#endif

  if(background){
    JASSERT_STDERR  << "Backgrounding...\n";
    JASSERT(dup2(open("/dev/null",O_RDWR), 0)==0);
    fflush(stdout);
    JASSERT(close(1)==0);
    JASSERT(open("/dev/null", O_WRONLY)==1);
    fflush(stderr);
    JASSERT (close(2) == 0 && dup2(1,2) == 2) .Text( "Can't print to stderr");
    close(JASSERT_STDERR_FD);
    dup2(2, JASSERT_STDERR_FD);
    if(fork()>0){
      JTRACE ( "Parent Exiting after fork()" );
      exit(0);
    }
    //pid_t sid = setsid();
  } else if ( batchMode ) {
    JASSERT_STDERR  << "Going into Batch Mode...\n";
    close(0);
    close(1);
    close(2);
    close(JASSERT_STDERR_FD);

    JASSERT(open("/dev/null", O_WRONLY)==0);

    JASSERT(dup2(0, 1) == 1);
    JASSERT(dup2(0, 2) == 2);
    JASSERT(dup2(0, JASSERT_STDERR_FD) == JASSERT_STDERR_FD);

  } else {
    JASSERT_STDERR  <<
      "Type '?' for help." <<
      "\n\n";
  }

  /* We set up the signal handler for SIGINT so that it would send the
   * DMT_KILL_PEER message to all the connected peers before exiting.
   */
  setupSIGINTHandler();
  prog.addListenSocket ( *sock );
  if(!background && !batchMode)
    prog.addDataSocket ( new jalib::JChunkReader ( STDIN_FD , 1 ) );

  prog.monitorSockets ( theCheckpointInterval );
  return 0;
}
