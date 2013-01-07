/****************************************************************************
 *   Copyright (C) 2006-2008 by Jason Ansel, Kapil Arya, Gene Cooperman,    *
 *                                                           and Rohan Garg *
 *   jansel@csail.mit.edu, kapil@ccs.neu.edu, gene@ccs.neu.edu, and         *
 *                                                      rohgarg@ccs.neu.edu *
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

#ifndef DMTCPCONNECTION_H
#define DMTCPCONNECTION_H

// THESE INCLUDES ARE IN RANDOM ORDER.  LET'S CLEAN IT UP AFTER RELEASE. - Gene
#include "constants.h"
#include "dmtcpalloc.h"
#include "connectionidentifier.h"
#include "syscallwrappers.h"
#include <vector>
#include <sys/types.h>
#include <sys/socket.h>
#include <signal.h>
#include <map>
#include "../jalib/jbuffer.h"
#include "../jalib/jserialize.h"
#include "../jalib/jassert.h"
#include "../jalib/jconvert.h"
#include "../jalib/jalloc.h"
#include "../jalib/jfilesystem.h"
#include  "../jalib/jsocket.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mqueue.h>
#include <stdint.h>

#ifdef HAVE_SYS_INOTIFY_H
#include <sys/inotify.h>
#endif

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif
#ifdef HAVE_EPOLL_H
# include <sys/epoll.h>
#else
/* KEEP THIS IN SYNC WITH syscallwrappers.h */
# ifndef _SYS_EPOLL_H
#  define _SYS_EPOLL_H    1
struct epoll_event {int dummy;};
/* Valid opcodes("op" parameter) to issue to epoll_ctl().  */
#  define EPOLL_CTL_ADD 1 /* Add a file decriptor to the interface.  */
#  define EPOLL_CTL_DEL 2 /* Remove a file decriptor from the interface.  */
#  define EPOLL_CTL_MOD 3 /* Change file decriptor epoll_event structure.  */
# endif
#endif
#ifdef HAVE_EVENTFD_H
# include <sys/eventfd.h>
#else
enum { EFD_SEMAPHORE = 1 };
#endif
#ifdef HAVE_SIGNALFD_H
# include <sys/signalfd.h>
#else
# include <stdint.h>
struct signalfd_siginfo {uint32_t ssi_signo; int dummy;};
#endif

namespace dmtcp
{

  class KernelBufferDrainer;
  class ConnectionRewirer;
  class TcpConnection;
  class EpollConnection;
#ifdef DMTCP_USE_INOTIFY
  class InotifyConnection;
#endif


  class Connection
  {
    public:
#ifdef JALIB_ALLOCATOR
      static void* operator new(size_t nbytes, void* p) { return p; }
      static void* operator new(size_t nbytes) { JALLOC_HELPER_NEW(nbytes); }
      static void  operator delete(void* p) { JALLOC_HELPER_DELETE(p); }
#endif
      enum ConnectionType
      {
        INVALID  = 0x00000,
        TCP      = 0x10000,
        RAW      = 0x11000,
        PTY      = 0x20000,
        FILE     = 0x21000,
        STDIO    = 0x22000,
        FIFO     = 0x24000,
        EPOLL    = 0x30000,
        EVENTFD  = 0x31000,
        SIGNALFD = 0x32000,
        INOTIFY  = 0x34000,
        POSIXMQ  = 0x40000,
        TYPEMASK = TCP | RAW | PTY | FILE | STDIO | FIFO | EPOLL | EVENTFD |
          SIGNALFD | INOTIFY | POSIXMQ
      };

      Connection() {}
      virtual ~Connection() {}

      void addFd(int fd);
      void removeFd(int fd);
      size_t numFds() const { return _fds.size(); }
      const vector<int>& getFds() const { return _fds; }
      int  conType() const { return _type & TYPEMASK; }
      int  subType() const { return _type; }
      bool restoreInSecondIteration() { return _restoreInSecondIteration; }
      bool hasLock() { return _hasLock; }

      void  checkLock();
      const ConnectionIdentifier& id() const { return _id; }

      virtual void preCheckpoint(KernelBufferDrainer&) = 0;
      virtual void postRefill(bool isRestart = false) = 0;
      virtual void restore(ConnectionRewirer *rewirer = NULL) = 0;

      virtual void doLocking();
      virtual void saveOptions();
      virtual void restoreOptions();

      virtual void doSendHandshakes(const dmtcp::UniquePid& coordinator) {};
      virtual void doRecvHandshakes(const dmtcp::UniquePid& coordinator) {};

      //convert with type checking
      virtual TcpConnection& asTcp();
      virtual EpollConnection& asEpoll();
#ifdef DMTCP_USE_INOTIFY
      virtual InotifyConnection& asInotify();
#endif

      virtual string str() = 0;

      void serialize(jalib::JBinarySerializer& o);
    protected:
      virtual void serializeSubClass(jalib::JBinarySerializer& o) = 0;
    protected:
      //only child classes can construct us...
      Connection(int t);
    protected:
      ConnectionIdentifier _id;
      int                  _type;
      int                  _fcntlFlags;
      int                  _fcntlOwner;
      int                  _fcntlSignal;
      bool                 _hasLock;
      bool                 _restoreInSecondIteration;
      vector<int>          _fds;
  };

  class SocketConnection
  {
    public:
      enum PeerType
      {
        PEER_UNKNOWN,
        PEER_INTERNAL,
        PEER_EXTERNAL,
        PEER_SOCKETPAIR
      };

      enum PeerType peerType() const { return _peerType; }

      SocketConnection() {}
      SocketConnection(int domain, int type, int protocol);
      void addSetsockopt(int level, int option, const char* value, int len);
      void restoreSocketOptions(dmtcp::vector<int>& fds);
      void serialize(jalib::JBinarySerializer& o);

    protected:
      int _sockDomain;
      int _sockType;
      int _sockProtocol;
      enum PeerType _peerType;
      bool          _socketPairRestored;
      map< int, map<int, jalib::JBuffer> > _sockOptions;
  };

  class TcpConnection : public Connection, public SocketConnection
  {
    public:
      enum TcpType
      {
        TCP_INVALID = TCP,
        TCP_ERROR,
        TCP_CREATED,
        TCP_BIND,
        TCP_LISTEN,
        TCP_ACCEPT,
        TCP_CONNECT,
        TCP_PREEXISTING,
        TCP_EXTERNAL_CONNECT
      };

      TcpConnection() {}
      int tcpType() const { return _type; }

      // This accessor is needed because _type is protected.
      void markExternalConnect() { _type = TCP_EXTERNAL_CONNECT; }

      //basic commands for updating state from wrappers
      /*onSocket*/
      TcpConnection(int domain, int type, int protocol);
      void onBind(int sockfd, const struct sockaddr* addr, socklen_t len);
      void onListen(int backlog);
      void onConnect(int sockfd = -1, const struct sockaddr *serv_addr = NULL,
                     socklen_t addrlen = 0);
      /*onAccept*/
      TcpConnection(const TcpConnection& parent,
                    const ConnectionIdentifier& remote);
      void onError();
      void onDisconnect();

      void markPreExisting() { _type = TCP_PREEXISTING; }

      //basic checkpointing commands
      virtual void preCheckpoint(KernelBufferDrainer& drain);
      virtual void postRefill(bool isRestart = false);
      virtual void restore(ConnectionRewirer *rewirer = NULL);
      virtual void restoreOptions();

      virtual void doSendHandshakes(const dmtcp::UniquePid& coordinator);
      virtual void doRecvHandshakes(const dmtcp::UniquePid& coordinator);

      void sendHandshake(int remotefd, const UniquePid& coordinator);
      void recvHandshake(int remotefd, const UniquePid& coordinator);

      void setSocketpairPeer(ConnectionIdentifier id) {
        _peerType = PEER_SOCKETPAIR;
        _socketpairPeerId = id;
      }

      void restoreSocketPair(dmtcp::TcpConnection *peer);
      const ConnectionIdentifier& getRemoteId() const
      { return _remotePeerId;}
      const ConnectionIdentifier& getSocketpairPeerId() const
      { return _socketpairPeerId; }
      virtual string str() { return "<TCP Socket>"; }

      virtual void serializeSubClass(jalib::JBinarySerializer& o);
    private:
      TcpConnection& asTcp();
    private:
      int                     _listenBacklog;
      union {
        socklen_t               _bindAddrlen;
        socklen_t               _connectAddrlen;
      };
      union {
        /* See 'man socket.h' or POSIX for 'struct sockaddr_storage' */
        struct sockaddr_storage _bindAddr;
        struct sockaddr_storage _connectAddr;
      };
      ConnectionIdentifier    _remotePeerId;
      ConnectionIdentifier    _socketpairPeerId;
  };

  class RawSocketConnection : public Connection, public SocketConnection
  {
    public:
      RawSocketConnection() {};
      //basic commands for updating state from wrappers
      RawSocketConnection(int domain, int type, int protocol);

      //basic checkpointing commands
      virtual void preCheckpoint(KernelBufferDrainer& drain);
      virtual void postRefill(bool isRestart = false);
      virtual void restore(ConnectionRewirer *rewirer = NULL);
      virtual void restoreOptions();

      virtual void serializeSubClass(jalib::JBinarySerializer& o);
      virtual string str() { return "<TCP Socket>"; }
    private:
      dmtcp::map< int, dmtcp::map< int, jalib::JBuffer > > _sockOptions;
  };

  class PtyConnection : public Connection
  {
    public:
      enum PtyType
      {
        PTY_INVALID   = PTY,
        PTY_DEV_TTY,
        PTY_CTTY,
        PTY_MASTER,
        PTY_SLAVE,
        PTY_BSD_MASTER,
        PTY_BSD_SLAVE

          //        TYPEMASK = PTY_CTTY | PTY_Master | PTY_Slave
      };

      PtyConnection() {}
      PtyConnection(int fd, const char *path, int flags, mode_t mode, int type);

      int  ptyType() { return _type;}// & TYPEMASK);
      dmtcp::string ptsName() { return _ptsName;; }
      dmtcp::string virtPtsName() { return _virtPtsName;; }

      virtual void preCheckpoint(KernelBufferDrainer& drain);
      virtual void postRefill(bool isRestart = false);
      virtual void restore(ConnectionRewirer *rewirer = NULL);
      virtual void serializeSubClass(jalib::JBinarySerializer& o);
      virtual string str() { return _masterName + ":" + _ptsName; }
    private:
      //PtyType   _type;
      dmtcp::string _masterName;
      dmtcp::string _ptsName;
      dmtcp::string _virtPtsName;
      int           _flags;
      mode_t        _mode;
      bool          _ptmxIsPacketMode;

  };

  class StdioConnection : public Connection
  {
    public:
      enum StdioType
      {
        STDIO_IN = STDIO,
        STDIO_OUT,
        STDIO_ERR,
        STDIO_INVALID
      };

      StdioConnection(int fd): Connection(STDIO + fd)
    {
      JTRACE("creating stdio connection") (fd) (id());
      JASSERT(jalib::Between(0, fd, 2)) (fd)
        .Text("invalid fd for StdioConnection");
    }

      StdioConnection() {}

      virtual void preCheckpoint(KernelBufferDrainer& drain);
      virtual void postRefill(bool isRestart = false);
      virtual void restore(ConnectionRewirer *rewirer = NULL);

      virtual string str() { return "<STDIO>"; };
      virtual void serializeSubClass(jalib::JBinarySerializer& o);
  };

  class FileConnection : public Connection
  {
    public:
      enum FileType
      {
        FILE_INVALID = FILE,
        FILE_REGULAR,
        FILE_PROCFS,
        FILE_DELETED,
        FILE_RESMGR
      };

      enum ResMgrFileType
      {
        TORQUE_IO,
        TORQUE_NODE
      };

      FileConnection() {}
      FileConnection(const dmtcp::string& path, int flags, mode_t mode,
                     int type = FILE_REGULAR)
        : Connection(FILE)
        , _path(path)
        , _flags(flags)
        , _mode(mode)
      {
         _type = type;
      }

      virtual void doLocking();
      virtual void preCheckpoint(KernelBufferDrainer& drain);
      virtual void postRefill(bool isRestart = false);
      virtual void restore(ConnectionRewirer *rewirer = NULL);

      virtual void serializeSubClass(jalib::JBinarySerializer& o);

      virtual string str() { return _path; }
      void restoreFile(dmtcp::string newpath = "", bool check_exist = true);
      dmtcp::string filePath() { return _path; }
      bool checkpointed() { return _checkpointed; }
      void doNotRestoreCkptCopy() {
        _checkpointed = false; _restoreInSecondIteration = true;
      }

      int fileType() { return _type; }

    private:
      void saveFile(int fd);
      int  openFile();
      void refreshPath();
      void handleUnlinkedFile();
      void calculateRelativePath();
      dmtcp::string getSavedFilePath(const dmtcp::string& path);
      void preCheckpointResMgrFile();
      bool restoreResMgrFile();

      dmtcp::string _path;
      dmtcp::string _rel_path;
      dmtcp::string _ckptFilesDir;
      bool          _checkpointed;
      int           _flags;
      mode_t        _mode;
      off_t         _offset;
      struct stat   _stat;
      ResMgrFileType _rmtype;
  };

  class FifoConnection : public Connection
  {
    public:

      FifoConnection() {}
      FifoConnection(const dmtcp::string& path, int flags, mode_t mode)
        : Connection(FIFO)
          , _path(path)
    {
      dmtcp::string curDir = jalib::Filesystem::GetCWD();
      int offs = _path.find(curDir);
      if (offs < 0) {
        _rel_path = "*";
      } else {
        offs += curDir.size();
        offs = _path.find('/',offs);
        offs++;
        _rel_path = _path.substr(offs);
      }
      JTRACE("New Fifo connection created") (_path) (_rel_path);
      _in_data.clear();
    }

      virtual void preCheckpoint(KernelBufferDrainer& drain);
      virtual void postRefill(bool isRestart = false);
      virtual void restore(ConnectionRewirer *rewirer = NULL);

      virtual string str() { return _path; };
      virtual void serializeSubClass(jalib::JBinarySerializer& o);

    private:
      int  openFile();
      void refreshPath();
      dmtcp::string getSavedFilePath(const dmtcp::string& path);
      dmtcp::string _path;
      dmtcp::string _rel_path;
      dmtcp::string _savedRelativePath;
      int           _flags;
      mode_t        _mode;
      struct stat _stat;
      vector<char> _in_data;
      int ckptfd;
  };

  class EpollConnection: public Connection
  {
    public:
      enum EpollType
      {
        EPOLL_INVALID = EPOLL,
        EPOLL_CREATE,
        EPOLL_CTL,
        EPOLL_WAIT
      };

      inline EpollConnection(int size, int type=EPOLL_CREATE)
        :Connection(EPOLL),
        _type(type),
        _size(size)
    {
      JTRACE("new epoll connection created");
    }

      int epollType() const { return _type; }

      virtual void preCheckpoint(KernelBufferDrainer&);
      virtual void postRefill(bool isRestart = false);
      virtual void restore(ConnectionRewirer *rewirer = NULL);
      virtual void serializeSubClass(jalib::JBinarySerializer& o);

      virtual string str() { return "EPOLL-FD: <Not-a-File>"; };

      void onCTL(int op, int fd, struct epoll_event *event);

    private:
      EpollConnection& asEpoll();
      int         _type; // current state of EPOLL
      struct stat _stat; // not sure if stat makes sense in case  of epfd
      int         _size; // flags
      dmtcp::map<int, struct epoll_event > _fdToEvent;
  };

  class EventFdConnection: public Connection
  {
    public:
      inline EventFdConnection(unsigned int initval, int flags)
        :Connection(EVENTFD),
        _initval(initval),
        _flags(flags)
    {
      JTRACE("new eventfd connection created");
    }

      virtual void preCheckpoint(KernelBufferDrainer&);
      virtual void postRefill(bool isRestart = false);
      virtual void restore(ConnectionRewirer *rewirer = NULL);
      virtual void serializeSubClass(jalib::JBinarySerializer& o);

      virtual string str() { return "EVENT-FD: <Not-a-File>"; };

    private:
      unsigned int   _initval; // initial counter value
      int         _flags; // flags
      int evtfd;
  };

  class SignalFdConnection: public Connection
  {
    public:
      inline SignalFdConnection(int signalfd, const sigset_t* mask, int flags)
        :Connection(SIGNALFD),
        signlfd(signalfd),
        _flags(flags)
    {
      if (mask!=NULL)
        _mask = *mask;
      else
        sigemptyset(&_mask);
      memset(&_fdsi, 0, sizeof(_fdsi));
      JTRACE("new signalfd  connection created");
    }

      virtual void preCheckpoint(KernelBufferDrainer&);
      virtual void postRefill(bool isRestart = false);
      virtual void restore(ConnectionRewirer *rewirer = NULL);
      virtual void serializeSubClass(jalib::JBinarySerializer& o);

      virtual string str() { return "SIGNAL-FD: <Not-a-File>"; };

    private:
      int signlfd;
      int         _flags; // flags
      sigset_t _mask; // mask for signals
      struct signalfd_siginfo _fdsi;
  };

#ifdef DMTCP_USE_INOTIFY
  class InotifyConnection: public Connection
  {
    public:
      enum InotifyState {
        INOTIFY_INVALID = INOTIFY,
        INOTIFY_CREATE,
        INOTIFY_ADD_WAIT
      };

      inline InotifyConnection (int flags)
          :Connection(INOTIFY),
           _flags (flags),
           _state(INOTIFY_CREATE)
      {
        JTRACE ("new inotify connection created");
      }

      int inotifyState() const { return _state; }
      InotifyConnection& asInotify();

      virtual void preCheckpoint(KernelBufferDrainer&);
      virtual void postRefill(bool isRestart = false);
      virtual void restore(ConnectionRewirer *rewirer = NULL);
      virtual void serializeSubClass(jalib::JBinarySerializer& o);

      virtual string str() { return "INOTIFY-FD: <Not-a-File>"; };

      void map_inotify_fd_to_wd( int fd, int wd);
      void add_watch_descriptors(int wd, int fd, const char *pathname,
                                 uint32_t mask);
      void remove_watch_descriptors(int wd);
    private:
      int         _flags; // flags
      int         _state; // current state of INOTIFY
      struct stat _stat; // not sure if stat makes sense in case  of epfd
  };
#endif

  class PosixMQConnection: public Connection
  {
    public:
      inline PosixMQConnection(const char *name, int oflag, mode_t mode,
                               struct mq_attr *attr)
        : Connection(POSIXMQ)
          , _name(name)
          , _oflag(oflag)
          , _mode(mode)
          , _qnum(0)
          , _notifyReg(false)
    {
      if (attr != NULL) {
        _attr = *attr;
      }
    }

      virtual void preCheckpoint(KernelBufferDrainer&);
      virtual void postRefill(bool isRestart = false);
      virtual void restore(ConnectionRewirer *rewirer = NULL);

      virtual void serializeSubClass(jalib::JBinarySerializer& o);

      virtual string str() { return _name; }

      void on_mq_close();
      void on_mq_notify(const struct sigevent *sevp);

    private:
      dmtcp::string  _name;
      int            _oflag;
      mode_t         _mode;
      struct mq_attr _attr;
      long           _qnum;
      bool           _notifyReg;
      struct sigevent _sevp;
      dmtcp::vector<jalib::JBuffer> _msgInQueue;
      dmtcp::vector<unsigned> _msgInQueuePrio;
  };
}

#endif
