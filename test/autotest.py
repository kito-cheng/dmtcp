#!/usr/bin/env python
from random import randint
from time   import sleep
from os     import listdir
import subprocess
import pty
import socket
import os
import sys
import errno
import signal
import resource
import pwd
import stat
import re

signal.alarm(1800)  # half hour

if sys.version_info[0] != 2 or sys.version_info[0:2] < (2,4):
  print "test/autotest.py works only with Python 2.x for 2.x greater than 2.3"
  print "Change the beginning of test/autotest.py if you believe you can run."
  sys.exit(1)

#get testconfig
# This assumes Makefile.in in main dir, but only Makefile in test dir.
os.system("test -f Makefile || ./configure")
import testconfig

#number of checkpoint/restart cycles
CYCLES=2

#Number of times to try dmtcp_restart
RETRIES=2

#Sleep after each program startup (sec)
DEFAULT_S=0.3
if sys.version_info[0] == 2 and sys.version_info[0:2] >= (2,7) and \
    subprocess.check_output(['uname', '-p'])[0:3] == 'arm':
  DEFAULT_S *= 2

if testconfig.MTCP_USE_PROC_MAPS == "yes":
  DEFAULT_S = 2*DEFAULT_S
S=DEFAULT_S
#Appears as S*SLOW in code.  If --slow, then SLOW=5
SLOW=1

#Max time to wait for ckpt/restart to finish (sec)
TIMEOUT=10
# Raise this value when /usr/lib/locale/locale-archive is 100MB.
# This can happen on Red Hat-derived distros.
if os.path.exists("/usr/lib/locale/locale-archive") and \
   os.path.getsize("/usr/lib/locale/locale-archive") > 10e6:
  TIMEOUT *= int( os.path.getsize("/usr/lib/locale/locale-archive") / 10e6 )

#Interval between checks for ckpt/restart complete
INTERVAL=0.1

#Buffers for process i/o
BUFFER_SIZE=4096*8

#False redirects process stderr
VERBOSE=False

#Run (most) tests with user default (usually with gzip enable)
GZIP=os.getenv('DMTCP_GZIP') or "1"

#Warn cant create a file of size:
REQUIRE_MB=50

#Binaries
BIN="./bin/"

#parse program args
args={}
for i in sys.argv:
  args[i]=True
  if i=="-v" or i=="--verbose":
    VERBOSE=True
  if i=="--stress":
    CYCLES=100000
  if i=="--slow":
    SLOW=5
  if i=="-h" or i=="--help":
    print ("USAGE "+sys.argv[0]+
      " [-v] [--stress] [--slow] [testname] [testname ...]")
    sys.exit(1)

stats = [0, 0]

def xor(bool1, bool2):
  return (bool1 or bool2) and (not bool1 or not bool2)

def replaceChar(string, index, char):
  return string[0:index] + char + string[index+1:len(string)]

def splitWithQuotes(string):
  inSingleQuotes = False
  inDoubleQuotes = False
  isOuter = False
  escapeChar = False
  for i in range(len(string)):
    if escapeChar:
      escapeChar = False
      continue
    if string[i] == "\\":
      escapeChar = True
      # Remove one level of escaping if same quoting char as isOuter
      string = replaceChar(string, i, '#')
      continue
    if string[i] == "'":
      inSingleQuotes = not inSingleQuotes
    if string[i] == '"':
      inDoubleQuotes = not inDoubleQuotes
    # Remove outermost quotes: 'bash -c "sleep 30"' => ['bash','-c','sleep 30']
    if string[i] == "'" or string[i] == '"':
      # This triggers twice in:  '"..."'  (on first ' and second ")
      if xor(inSingleQuotes, inDoubleQuotes) and not isOuter: # if beg. of quote
	isOuter = string[i]
        string = replaceChar(string, i, '#')
      elif isOuter == string[i]:  # if end of quote
	isOuter = False
        string = replaceChar(string, i, '#')
    if not inSingleQuotes and not inDoubleQuotes and string[i] == ' ':
      # FIXME (Is there any destructive way to do this?)
      string = replaceChar(string, i, '%')
  string = string.replace('#', '')
  return string.split('%')

def shouldRunTest(name):
  # FIXME:  This is a hack.  We should have created var, testNaems and use here
  if len(sys.argv) <= 1+(VERBOSE==True)+(SLOW!=1)+(CYCLES!=2):
    return True
  return name in sys.argv

#make sure we are in svn root
if os.system("test -d bin") is not 0:
  os.chdir("..")
assert os.system("test -d bin") is 0

#make sure dmtcp is built
if os.system("make -s --no-print-directory tests") != 0:
  print "`make all tests` FAILED"
  sys.exit(1)

#pad a string and print/flush it
def printFixed(str, w=1):
  # The comma at end of print prevents a "newline", but still adds space.
  print str.ljust(w),
  sys.stdout.flush()

#exception on failed check
class CheckFailed(Exception):
  def __init__(self, value=""):
    self.value = value

class MySubprocess:
  "dummy class: same fields as from subprocess module"
  def __init__(self, pid):
    self.pid = pid
    self.stdin = os.open(os.devnull, os.O_RDONLY)
    self.stdout = os.open(os.devnull, os.O_WRONLY)
    self.stderr = os.open(os.devnull, os.O_WRONLY)

def master_read(fd):
  os.read(fd, 4096)
  return ''

#launch a child process
# NOTE:  Can eventually migrate to Python 2.7:  subprocess.check_output
devnullFd = os.open(os.devnull, os.O_WRONLY)
def launch(cmd):
  global devnullFd
  global master_read
  if VERBOSE:
    print "Launching... ", cmd
  cmd = splitWithQuotes(cmd);
  # Example cmd:  dmtcp_checkpoint screen ...
  ptyMode = False
  for str in cmd:
    # Checkpoint image can be emacs23_x, or whatever emacs is a link to.
    # vim can be vim.gnome, etc.
    if re.search("(_|/|^)(screen|script|vim.*|emacs.*|pty)(_|$)", str):
      ptyMode = True
  try:
    os.stat(cmd[0])
  except:
    raise CheckFailed(cmd[0] + " not found")
  if ptyMode:
    # FOR DEBUGGING:  This can mysteriously fail, causing pty.fork() to fail
    try:
      (fd1, fd2) = os.openpty()
    except OSError, e:
      print "\n\n/dev/ptmx:"; os.system("ls -l /dev/ptmx /dev/pts")
      raise e
    else:
      os.close(fd1); os.close(fd2)
    (pid, fd) = pty.fork()
    if pid == 0:
      # Close all fds except stdin/stdout/stderr
      os.closerange(3,1024)
      signal.alarm(300) # pending alarm inherited across exec, but not a fork
      # Problem:  pty.spawn invokes fork.  alarm() will have no effect.
      pty.spawn(cmd, master_read)
      sys.exit(0)
    else:
      return MySubprocess(pid)
  else:
    if cmd[0] == BIN+"dmtcp_coordinator":
      childStdout = subprocess.PIPE
      # Don't mix stderr in with childStdout; need to read stdout
      if VERBOSE:
        childStderr = None
      else:
        childStderr = devnullFd
    elif VERBOSE:
      childStdout=None  # Inherit child stdout from parent
      childStderr=None  # Inherit child stderr from parent
    else:
      childStdout = devnullFd
      childStderr = subprocess.STDOUT # Mix stderr into stdout file object
    # NOTE:  This might be replaced by shell=True in call to subprocess.Popen
    proc = subprocess.Popen(cmd, bufsize=BUFFER_SIZE,
		 stdin=subprocess.PIPE, stdout=childStdout,
		 stderr=childStderr, close_fds=True)
  return proc

#randomize port and dir, so multiple processes works
ckptDir="dmtcp-autotest-%d" % randint(100000000,999999999)
os.mkdir(ckptDir);
os.environ['DMTCP_HOST'] = "localhost"
os.environ['DMTCP_PORT'] = str(randint(2000,10000))
os.environ['DMTCP_CHECKPOINT_DIR'] = os.path.abspath(ckptDir)
#Use default SIGCKPT for test suite.
os.unsetenv('DMTCP_SIGCKPT')
os.unsetenv('MTCP_SIGCKPT')
#No gzip by default.  (Isolate gzip failures from other test failures.)
#But note that dmtcp3, frisbee and gzip tests below still use gzip.
if not VERBOSE:
  os.environ['JALIB_STDERR_PATH'] = os.devnull
if VERBOSE:
  print "coordinator port:  " + os.environ['DMTCP_PORT']

#verify there is enough free space
tmpfile=ckptDir + "/freeSpaceTest.tmp"
if os.system("dd if=/dev/zero of="+tmpfile+" bs=1MB count="+str(REQUIRE_MB)+" 2>/dev/null") != 0:
  GZIP="1"
  print '''

!!!WARNING!!!
Fewer than '''+str(REQUIRE_MB)+'''MB are available on the current volume.
Many of the tests below may fail due to insufficient space.
!!!WARNING!!!

'''
os.system("rm -f "+tmpfile)

os.environ['DMTCP_GZIP'] = GZIP

#launch the coordinator
coordinator = launch(BIN+"dmtcp_coordinator")

#send a command to the coordinator process
def coordinatorCmd(cmd):
  try:
    if VERBOSE and cmd != "s":
      print "COORDINATORCMD(",cmd,")"
    coordinator.stdin.write(cmd+"\n")
    coordinator.stdin.flush()
  except:
    raise CheckFailed("failed to write '%s' to coordinator (pid: %d)" %  (cmd, coordinator.pid))

#clean up after ourselves
def SHUTDOWN():
  try:
    coordinatorCmd('q')
    sleep(S*SLOW)
  except:
    print "SHUTDOWN() failed"
  os.system("kill -9 %d" % coordinator.pid)
  os.system("rm -rf  %s" % ckptDir)
  os.close(devnullFd)

#make sure val is true
def CHECK(val, msg):
  if not val:
    raise CheckFailed(msg)

#wait TIMEOUT for test() to be true, or throw error
def WAITFOR(test, msg):
  left=TIMEOUT*(S/DEFAULT_S)/INTERVAL
  while not test():
    if left <= 0:
      CHECK(False, msg())
    left-=1
    sleep(INTERVAL)

#extract (NUM_PEERS, RUNNING) from coordinator
def getStatus():
  coordinatorCmd('s')

  if coordinator.poll() >= 0:
    CHECK(False, "coordinator died unexpectedly")
    return (-1, False)

  while True:
    try:
      line=coordinator.stdout.readline().strip()
      if line=="Status...":
        break;
      if VERBOSE:
        print "Ignoring line from coordinator: ", line
    except IOError, (errno, strerror):
      if coordinator.poll() >= 0:
        CHECK(False, "coordinator died unexpectedly")
        return (-1, False)
      if errno==4: #Interrupted system call
        continue
      raise CheckFailed("I/O error(%s): %s" % (errno, strerror))

  x,peers=coordinator.stdout.readline().strip().split("=")
  CHECK(x=="NUM_PEERS", "reading coordinator status")
  x,running=coordinator.stdout.readline().strip().split("=")
  CHECK(x=="RUNNING", "reading coordinator status")

  if VERBOSE:
    print "STATUS: peers=%s, running=%s" % (peers,running)
  return (int(peers), (running=="yes"))

#delete all files in ckptDir
def clearCkptDir():
  for TRIES in range(2):  # Try twice in case ckpt_*_dmtcp.temp is renamed.
    #clear checkpoint dir
    for root, dirs, files in os.walk(ckptDir, topdown=False):
      for name in files:
        try:
          # if name.endswith(".dmtcp") :
          #   import shutil
          #   shutil.copy(os.path.join(root, name), "/home/kapil/dmtcp/ramfs")
          # else:
          #   os.remove(os.path.join(root, name))
          os.remove(os.path.join(root, name))
        except OSError, e:
	  if e.errno != errno.ENOENT:  # Maybe ckpt_*_dmtcp.temp was renamed.
	    raise e
      for name in dirs:
        os.rmdir(os.path.join(root, name))

def getNumCkptFiles(dir):
  return len(filter(lambda f: f.startswith("ckpt_") and f.endswith(".dmtcp"), listdir(dir)))


# Test a given list of commands to see if they checkpoint
# runTest() sets up a keyboard interrupt handler, and then calls this function.
def runTestRaw(name, numProcs, cmds):
  #the expected/correct running status
  if testconfig.USE_M32 == "1":
    def forall(fnc, lst):
      return reduce(lambda x, y: x and y, map(fnc, lst))
    if not forall(lambda x: x.startswith("./test/"), cmds):
      return
  status=(numProcs, True)
  procs=[]

  def doesStatusSatisfy(newStatus,requiredStatus):
    if isinstance(requiredStatus[0], int):
      statRange = [requiredStatus[0]]
    elif isinstance(requiredStatus[0], list):
      statRange = requiredStatus[0]
    else:
      raise NotImplementedError
    return newStatus[0] in statRange and newStatus[1] == requiredStatus[1]

  def wfMsg(msg):
    #return function to generate error message
    return lambda: msg+", "+str(status[0])+ \
                   " expected, %d found, running=%d" % getStatus()

  def testKill():
    #kill all processes
    coordinatorCmd('k')
    try:
      WAITFOR(lambda: getStatus()==(0, False),
	      lambda:"coordinator kill command failed")
    except CheckFailed:
      global coordinator
      coordinatorCmd('q')
      os.system("kill -9 %d" % coordinator.pid)
      print "Trying to kill old coordinator, and launch new one on same port"
      coordinator = launch(BIN+"dmtcp_coordinator")
    for x in procs:
      #cleanup proc
      try:
        if isinstance(x.stdin,int):
          os.close(x.stdin)
        elif x.stdin:
          x.stdin.close()
        if isinstance(x.stdout,int):
          os.close(x.stdout)
        elif x.stdout:
          x.stdout.close()
        if isinstance(x.stderr,int):
          os.close(x.stderr)
        elif x.stderr:
          x.stderr.close()
      except:
        None
      try:
        os.waitpid(x.pid, os.WNOHANG)
      except OSError, e:
	if e.errno != errno.ECHILD:
	  raise e
      procs.remove(x)

  def testCheckpoint():
    #start checkpoint
    coordinatorCmd('c')

    #wait for files to appear and status to return to original
    WAITFOR(lambda: getNumCkptFiles(ckptDir)>0 and \
                    doesStatusSatisfy(getStatus(), status),
            wfMsg("checkpoint error"))

    #make sure the right files are there
    numFiles=getNumCkptFiles(ckptDir) # len(listdir(ckptDir))
    CHECK(doesStatusSatisfy((numFiles,True),status),
          "unexpected number of checkpoint files, %s procs, %d files"
          % (str(status[0]), numFiles))

  def testRestart():
    #build restart command
    cmd=BIN+"dmtcp_restart --quiet"
    for i in listdir(ckptDir):
      if i.endswith(".dmtcp"):
        cmd+= " "+ckptDir+"/"+i
    #run restart and test if it worked
    procs.append(launch(cmd))
    WAITFOR(lambda: doesStatusSatisfy(getStatus(), status),
            wfMsg("restart error"))
    if testconfig.HBICT_DELTACOMP == "no":
      clearCkptDir()

  try:
    printFixed(name,15)

    if not shouldRunTest(name):
      print "SKIPPED"
      return

    stats[1]+=1
    CHECK(getStatus()==(0, False), "coordinator initial state")

    #start user programs
    for cmd in cmds:
      procs.append(launch(BIN+"dmtcp_checkpoint "+cmd))
      sleep(S*SLOW)

    WAITFOR(lambda: doesStatusSatisfy(getStatus(), status),
            wfMsg("user program startup error"))

    for i in range(CYCLES):
      if i!=0 and i%2==0:
        print #newline
        printFixed("",15)
      printFixed("ckpt:")
      # NOTE:  If this faile, it will throw an exception to CheckFailed
      #  of this function:  testRestart
      testCheckpoint()
      printFixed("PASSED ")
      testKill()

      printFixed("rstr:")
      for j in range(RETRIES):
        try:
          testRestart()
          printFixed("PASSED")
          break
        except CheckFailed, e:
          if j == RETRIES-1:
            raise e
          else:
            printFixed("FAILED retry:")
            testKill()
      if i != CYCLES - 1:
	printFixed(";")

    testKill()
    print #newline
    stats[0]+=1

  except CheckFailed, e:
    print "FAILED"
    printFixed("",15)
    print "root-pids:", map(lambda x: x.pid, procs), "msg:", e.value
    try:
      testKill()
    except CheckFailed, e:
      print "CLEANUP ERROR:", e.value
      SHUTDOWN()
      saveResultsNMI()
      sys.exit(1)

  clearCkptDir()

def getProcessChildren(pid):
    p = subprocess.Popen("ps --no-headers -o pid --ppid %d" % pid, shell = True,
                         stdout = subprocess.PIPE, stderr = subprocess.PIPE)
    stdout, stderr = p.communicate()
    return [int(pid) for pid in stdout.split()]

# If the user types ^C, then kill all child processes.
def runTest(name, numProcs, cmds):
  try:
    runTestRaw(name, numProcs, cmds)
  except KeyboardInterrupt:
    for pid in getProcessChildren(os.getpid()):
      try:
        os.kill(pid, signal.SIGKILL)
      except OSError: # This happens if pid already died.
        pass

def saveResultsNMI():
  if testconfig.DEBUG == "yes":
    # WARNING:  This can cause a several second delay on some systems.
    host = socket.getfqdn()
    if re.search("^nmi-.*.cs.wisc.edu$", host) or \
       re.search("^nmi-.*.cs.wisconsin.edu$", host):
      tmpdir = os.getenv("TMPDIR", "/tmp") # if "TMPDIR" not set, return "/tmp"
      target = "./dmtcp-" + pwd.getpwuid(os.getuid()).pw_name + \
               "@" + socket.gethostname()
      cmd = "mkdir results; cp -pr " + tmpdir + "/" + target + \
	       " ./dmtcp/src/libdmtcp.so" + \
	       " ./dmtcp/src/dmtcp_coordinator" + \
               " ./mtcp/libmtcp.so" + \
               " results/"
      os.system(cmd)
      cmd = "tar zcf ../results.tar.gz ./results; rm -rf results"
      os.system(cmd)
      print "\n*** results.tar.gz ("+tmpdir+"/"+target+ \
					      ") written to DMTCP_ROOT/.. ***"

print "== Tests =="

#tmp port
p0=str(randint(2000,10000))
p1=str(randint(2000,10000))
p2=str(randint(2000,10000))
p3=str(randint(2000,10000))

# Use uniform user shell.  Else apps like script have different subprocesses.
os.environ["SHELL"]="/bin/bash"

runTest("dmtcp1",        1, ["./test/dmtcp1"])

runTest("dmtcp2",        1, ["./test/dmtcp2"])

runTest("dmtcp3",        1, ["./test/dmtcp3"])

runTest("dmtcp4",        1, ["./test/dmtcp4"])

# In 32-bit Ubuntu 9.10, the default small stacksize (8 MB) forces
# legacy_va_layout, which places vdso in low memory.  This collides with text
# in low memory (0x110000) in the statically linked mtcp_restart executable.
oldLimit = resource.getrlimit(resource.RLIMIT_STACK)
# oldLimit[1] is old hard limit
if oldLimit[1] == -1L:
  newCurrLimit = 8L*1024*1024
else:
  newCurrLimit = min(8L*1024*1024, oldLimit[1])
resource.setrlimit(resource.RLIMIT_STACK, [newCurrLimit, oldLimit[1]])
runTest("dmtcp5",        2, ["./test/dmtcp5"])
resource.setrlimit(resource.RLIMIT_STACK, oldLimit)

runTest("dmtcpaware1",   1, ["./test/dmtcpaware1"])

PWD=os.getcwd()
runTest("plugin-sleep2", 1, ["--with-plugin "+
			     PWD+"/test/plugin/sleep1/dmtcp_sleep1hijack.so:"+
			     PWD+"/test/plugin/sleep2/dmtcp_sleep2hijack.so "+
			     "./test/dmtcp1"])

runTest("plugin-example-db", 2, ["--with-plugin "+
			    PWD+"/test/plugin/example-db/dmtcp_example-dbhijack.so "+
			     "env EXAMPLE_DB_KEY=1 EXAMPLE_DB_KEY_OTHER=2 "+
			     "./test/dmtcp1",
			         "--with-plugin "+
			    PWD+"/test/plugin/example-db/dmtcp_example-dbhijack.so "+
			     "env EXAMPLE_DB_KEY=2 EXAMPLE_DB_KEY_OTHER=1 "+
			     "./test/dmtcp1"])

# Test special case:  gettimeofday can be handled within VDSO segment.
runTest("gettimeofday",  1, ["./test/gettimeofday"])

runTest("sigchild",      1, ["./test/sigchild"])

runTest("shared-fd",     2, ["./test/shared-fd"])

runTest("stale-fd",      2, ["./test/stale-fd"])

# Disable procfd1 until we fix readlink
#runTest("procfd1",       2, ["./test/procfd1"])

runTest("popen1",          1, ["./test/popen1"])

runTest("poll",          1, ["./test/poll"])

runTest("forkexec",      2, ["./test/forkexec"])

if testconfig.PID_VIRTUALIZATION == "yes":
  runTest("waitpid",      2, ["./test/waitpid"])

runTest("client-server", 2, ["./test/client-server"])

# frisbee creates three processes, each with 14 MB, if no gzip is used
os.environ['DMTCP_GZIP'] = "1"
runTest("frisbee",       3, ["./test/frisbee "+p1+" localhost "+p2,
                             "./test/frisbee "+p2+" localhost "+p3,
                             "./test/frisbee "+p3+" localhost "+p1+" starter"])
os.environ['DMTCP_GZIP'] = "0"

runTest("shared-memory", 2, ["./test/shared-memory"])

# This is arguably a bug in the Linux kernel 3.2 for ARM.
if sys.version_info[0] == 2 and sys.version_info[0:2] >= (2,7) and \
    subprocess.check_output(['uname', '-p'])[0:3] == 'arm':
  print "On ARM, there is a known issue with the sysv-shm test. Not running it."
else:
  runTest("sysv-shm1",     2, ["./test/sysv-shm1"])
  runTest("sysv-shm2",     2, ["./test/sysv-shm2"])
  runTest("sysv-sem",      2, ["./test/sysv-sem"])
  runTest("sysv-msg",      2, ["./test/sysv-msg"])

runTest("posix-mq1",      2, ["./test/posix-mq1"])
runTest("posix-mq2",      2, ["./test/posix-mq2"])

#Invoke this test when we drain/restore data in pty at checkpoint time.
# runTest("pty1",   2, ["./test/pty1"])
runTest("pty2",   2, ["./test/pty2"])

#Invoke this test when support for timers is added to DMTCP.
runTest("timer",   1, ["./test/timer"])
runTest("clock",   1, ["./test/clock"])

old_ld_library_path = os.getenv("LD_LIBRARY_PATH")
if old_ld_library_path:
    os.environ['LD_LIBRARY_PATH'] += ':' + os.getenv("PWD")+"/test:"+os.getenv("PWD")
else:
    os.environ['LD_LIBRARY_PATH'] = os.getenv("PWD")+"/test:"+os.getenv("PWD")
runTest("dlopen",        1, ["./test/dlopen"])
if old_ld_library_path:
  os.environ['LD_LIBRARY_PATH'] = old_ld_library_path
else:
  del os.environ['LD_LIBRARY_PATH']

runTest("pthread1",      1, ["./test/pthread1"])
runTest("pthread2",      1, ["./test/pthread2"])
S=3
runTest("pthread3",      1, ["./test/pthread2 80"])
S=DEFAULT_S
runTest("pthread4",      1, ["./test/pthread4"])
runTest("pthread5",      1, ["./test/pthread5"])

os.environ['DMTCP_GZIP'] = "1"
runTest("gzip",          1, ["./test/dmtcp1"])
os.environ['DMTCP_GZIP'] = GZIP

if testconfig.HAS_READLINE == "yes":
  runTest("readline",    1,  ["./test/readline"])

runTest("perl",          1, ["/usr/bin/perl"])

if testconfig.HAS_PYTHON == "yes":
  runTest("python",      1, ["/usr/bin/python"])

if testconfig.PID_VIRTUALIZATION == "yes":
  os.environ['DMTCP_GZIP'] = "0"
  runTest("bash",        2, ["/bin/bash --norc -c 'ls; sleep 30; ls'"])
  os.environ['DMTCP_GZIP'] = GZIP

if testconfig.HAS_DASH == "yes":
  os.environ['DMTCP_GZIP'] = "0"
  os.unsetenv('ENV')  # Delete reference to dash initialization file
  runTest("dash",        2, ["/bin/dash -c 'ls; sleep 30; ls'"])
  os.environ['DMTCP_GZIP'] = GZIP

if testconfig.HAS_TCSH == "yes":
  os.environ['DMTCP_GZIP'] = "0"
  runTest("tcsh",        2, ["/bin/tcsh -f -c 'ls; sleep 30; ls'"])
  os.environ['DMTCP_GZIP'] = GZIP

if testconfig.HAS_ZSH == "yes":
  os.environ['DMTCP_GZIP'] = "0"
  S=1
  runTest("zsh",         2, ["/bin/zsh -f -c 'ls; sleep 30; ls'"])
  S=DEFAULT_S
  os.environ['DMTCP_GZIP'] = GZIP

if testconfig.HAS_VIM == "yes" and testconfig.PID_VIRTUALIZATION == "yes":
  # Wait to checkpoint until vim finishes reading its initialization files
  S=3
  if sys.version_info[0:2] >= (2,6):
    # Delete previous vim processes.  Vim behaves poorly with stale processes.
    vimCommand = testconfig.VIM + " /etc/passwd +3" # +3 makes cmd line unique
    def killCommand(cmdToKill):
      if os.getenv('USER') == None:
        return
      ps = subprocess.Popen(['ps', '-u', os.environ['USER'], '-o', 'pid,command'],
    		            stdout=subprocess.PIPE).communicate()[0]
      for row in ps.split('\n')[1:]:
        cmd = row.split(None, 1) # maxsplit=1
        if cmd and cmd[1] == cmdToKill:
          os.kill(int(cmd[0]), signal.SIGKILL)
    killCommand(vimCommand)
    runTest("vim",       1,  ["env TERM=vt100 " + vimCommand])
    killCommand(vimCommand)
  S=DEFAULT_S

if testconfig.HAS_EMACS == "yes" and testconfig.PID_VIRTUALIZATION == "yes":
  # Wait to checkpoint until emacs finishes reading its initialization files
  S=4
  if sys.version_info[0:2] >= (2,6):
    # Under emacs23, it opens /dev/tty directly in a new fd.
    # To avoid this, consider using emacs --batch -l EMACS-LISTP-CODE ...
    # ... or else a better pty wrapper to capture emacs output to /dev/tty.
    runTest("emacs",     1,  ["env TERM=vt100 /usr/bin/emacs -nw" +
                              " --no-init-file /etc/passwd"])
  S=DEFAULT_S

if testconfig.HAS_SCRIPT == "yes" and testconfig.PID_VIRTUALIZATION == "yes":
  S=2
  if sys.version_info[0:2] >= (2,6):
    # NOTE: If 'script' fails, try raising value of S, above, to larger number.
    #  Arguably, there is a bug in glibc, in that locale-archive can be 100 MB.
    #  For example, in Fedora 13 (and other recent Red Hat-derived distros?),
    #  /usr/lib/locale/locale-archive is 100 MB, and yet 'locale -a |wc' shows
    #  only 8KB of content in ASCII.  The 100 MB of locale-archive condenses
    #  to 25 MB _per process_ under gzip, but this can be slow at ckpt time.
    runTest("script",    4,  ["/usr/bin/script -f" +
    			      " -c 'bash -c \"ls; sleep 30\"'" +
    			      " dmtcp-test-typescript.tmp"])
  os.system("rm -f dmtcp-test-typescript.tmp")
  S=DEFAULT_S

# SHOULD HAVE screen RUN SOMETHING LIKE:  bash -c ./test/dmtcp1
# FIXME: Currently fails on dekaksi due to DMTCP not honoring
#        "Async-signal-safe functions" in signal handlers (see man 7 signal)
if testconfig.HAS_SCREEN == "yes" and testconfig.PID_VIRTUALIZATION == "yes":
  S=1
  if sys.version_info[0:2] >= (2,6):
    runTest("screen",    3,  ["env TERM=vt100 " + testconfig.SCREEN +
                                " -c /dev/null -s /bin/sh"])
  S=DEFAULT_S

if testconfig.PTRACE_SUPPORT == "yes" and sys.version_info[0:2] >= (2,6):
  if testconfig.HAS_STRACE == "yes":
    S=3
    runTest("strace",    2,  ["strace test/dmtcp2"])
    S=DEFAULT_S

  if testconfig.HAS_GDB == "yes":
    os.system("echo 'run' > dmtcp-gdbinit.tmp")
    S=3
    runTest("gdb",          2, ["gdb -n -batch -x dmtcp-gdbinit.tmp test/dmtcp1"])

    runTest("gdb-pthread0", 2, ["gdb -n -batch -x dmtcp-gdbinit.tmp test/dmtcp3"])

    # These tests currently fail sometimes (if the computation is checkpointed
    # while a thread is being created). Re-enable them when this issue has been
    # fixed in the ptrace plugin.
    #runTest("gdb-pthread1", 2, ["gdb -n -batch -x dmtcp-gdbinit.tmp test/pthread1"])
    #runTest("gdb-pthread2",2, ["gdb -n -batch -x dmtcp-gdbinit.tmp test/pthread2"])

    S=DEFAULT_S
    os.system("rm -f dmtcp-gdbinit.tmp")

if testconfig.HAS_JAVAC == "yes" and testconfig.HAS_JAVA == "yes":
  S=3
  os.environ['CLASSPATH'] = './test'
  if testconfig.HAS_SUN_ORACLE_JAVA == "yes":
    runTest("java1",         1,  ["java -Xmx512M java1"])
  else:
    runTest("java1",         1,  ["java java1"])
  del os.environ['CLASSPATH']
  S=DEFAULT_S

if testconfig.HAS_CILK == "yes":
  runTest("cilk1",        1,  ["./test/cilk1 38"])

# SHOULD HAVE gcl RUN LARGE FACTORIAL OR SOMETHING.
if testconfig.HAS_GCL == "yes":
  S=1
  runTest("gcl",         1,  [testconfig.GCL])
  S=DEFAULT_S

if testconfig.HAS_OPENMP == "yes":
  runTest("openmp-1",         1,  ["./test/openmp-1"])
  runTest("openmp-2",         1,  ["./test/openmp-2"])

# SHOULD HAVE matlab RUN LARGE FACTORIAL OR SOMETHING.
if testconfig.HAS_MATLAB == "yes":
  S=3
  if sys.version_info[0:2] >= (2,6):
    runTest("matlab-nodisplay", 1,  [testconfig.MATLAB+" -nodisplay -nojvm"])
  S=DEFAULT_S

if testconfig.HAS_MPICH == "yes":
  runTest("mpd",         1, [testconfig.MPICH_MPD])

  runTest("hellompich-n1", 4, [testconfig.MPICH_MPD,
                           testconfig.MPICH_MPIEXEC+" -n 1 ./test/hellompich"])

  runTest("hellompich-n2", 6, [testconfig.MPICH_MPD,
                           testconfig.MPICH_MPIEXEC+" -n 2 ./test/hellompich"])

  runTest("mpdboot",     1, [testconfig.MPICH_MPDBOOT+" -n 1"])

  #os.system(testconfig.MPICH_MPDCLEANUP)

# Temporarily disabling OpenMPI test as it fails on some distros (OpenSUSE 11.4)
if testconfig.HAS_OPENMPI == "yes":
  numProcesses = 5 + int(testconfig.USES_OPENMPI_ORTED == "yes")
  # FIXME: Replace "[5,6]" by numProcesses when bug in configure is fixed.
  # /usr/bin/openmpi does not work if /usr/bin is not also in user's PATH
  oldPath = ""
  if not os.environ.has_key('PATH'):
    oldPath = None
    os.environ['PATH'] = os.path.dirname(testconfig.OPENMPI_MPIRUN)
  elif (not re.search(os.path.dirname(testconfig.OPENMPI_MPIRUN),
                     os.environ['PATH'])):
    oldPath = os.environ['PATH']
    os.environ['PATH'] += ":" + os.path.dirname(testconfig.OPENMPI_MPIRUN)
  S=1
  runTest("openmpi", [5,6], [testconfig.OPENMPI_MPIRUN + " -np 4" +
			     " ./test/openmpi"])
  S=DEFAULT_S
  if oldPath:
    os.environ['PATH'] = oldPath
  if oldPath == None:
    del os.environ['PATH']

print "== Summary =="
print "%s: %d of %d tests passed" % (socket.gethostname(), stats[0], stats[1])

saveResultsNMI()

try:
  SHUTDOWN()
except CheckFailed, e:
  print "Error in SHUTDOWN():", e.value
except:
  print "Error in SHUTDOWN()"

sys.exit( stats[1] - stats[0] )  # Return code is number of failing tests.
