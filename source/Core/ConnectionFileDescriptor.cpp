//===-- ConnectionFileDescriptor.cpp ----------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/Core/ConnectionFileDescriptor.h"

// C Includes
#include <errno.h>
#include <fcntl.h>
#ifdef _POSIX_SOURCE
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#endif

// C++ Includes
// Other libraries and framework includes
// Project includes
#include "lldb/lldb-private-log.h"
#include "lldb/Interpreter/Args.h"
#include "lldb/Core/Communication.h"
#include "lldb/Core/Log.h"
#include "lldb/Core/RegularExpression.h"
#include "lldb/Core/Timer.h"

using namespace lldb;
using namespace lldb_private;

static bool
DecodeHostAndPort (const char *host_and_port, 
                   std::string &host_str, 
                   std::string &port_str, 
                   int32_t& port,
                   Error *error_ptr)
{
    RegularExpression regex ("([^:]+):([0-9]+)");
    if (regex.Execute (host_and_port, 2))
    {
        if (regex.GetMatchAtIndex (host_and_port, 1, host_str) &&
            regex.GetMatchAtIndex (host_and_port, 2, port_str))
        {
            port = Args::StringToSInt32 (port_str.c_str(), INT32_MIN);
            if (port != INT32_MIN)
            {
                if (error_ptr)
                    error_ptr->Clear();
                return true;
            }
        }
    }
    host_str.clear();
    port_str.clear();
    port = INT32_MIN;
    if (error_ptr)
        error_ptr->SetErrorStringWithFormat("invalid host:port specification: '%s'", host_and_port);
    return false;
}

ConnectionFileDescriptor::ConnectionFileDescriptor () :
    Connection(),
    m_fd_send (-1),
    m_fd_recv (-1),
    m_fd_send_type (eFDTypeFile),
    m_fd_recv_type (eFDTypeFile),
    m_udp_send_sockaddr (),
    m_should_close_fd (false), 
    m_socket_timeout_usec(0),
    m_pipe_read(-1),
    m_pipe_write(-1),
    m_mutex (Mutex::eMutexTypeRecursive),
    m_shutting_down (false)
{
    LogSP log(lldb_private::GetLogIfAnyCategoriesSet (LIBLLDB_LOG_CONNECTION |  LIBLLDB_LOG_OBJECT));
    if (log)
        log->Printf ("%p ConnectionFileDescriptor::ConnectionFileDescriptor ()", this);
}

ConnectionFileDescriptor::ConnectionFileDescriptor (int fd, bool owns_fd) :
    Connection(),
    m_fd_send (fd),
    m_fd_recv (fd),
    m_fd_send_type (eFDTypeFile),
    m_fd_recv_type (eFDTypeFile),
    m_udp_send_sockaddr (),
    m_should_close_fd (owns_fd),
    m_socket_timeout_usec(0),
    m_pipe_read(-1),
    m_pipe_write(-1),
    m_mutex (Mutex::eMutexTypeRecursive),
    m_shutting_down (false)
{
    LogSP log(lldb_private::GetLogIfAnyCategoriesSet (LIBLLDB_LOG_CONNECTION |  LIBLLDB_LOG_OBJECT));
    if (log)
        log->Printf ("%p ConnectionFileDescriptor::ConnectionFileDescriptor (fd = %i, owns_fd = %i)", this, fd, owns_fd);
    OpenCommandPipe ();
}


ConnectionFileDescriptor::~ConnectionFileDescriptor ()
{
    LogSP log(lldb_private::GetLogIfAnyCategoriesSet (LIBLLDB_LOG_CONNECTION |  LIBLLDB_LOG_OBJECT));
    if (log)
        log->Printf ("%p ConnectionFileDescriptor::~ConnectionFileDescriptor ()", this);
    Disconnect (NULL);
    CloseCommandPipe ();
}

void
ConnectionFileDescriptor::OpenCommandPipe ()
{
    CloseCommandPipe();
    
    LogSP log(lldb_private::GetLogIfAnyCategoriesSet (LIBLLDB_LOG_CONNECTION |  LIBLLDB_LOG_OBJECT));
    // Make the command file descriptor here:
    int filedes[2];
#ifdef _POSIX_SOURCE
     int result = pipe (filedes);
#else
    int result = -1;
#endif
    if (result != 0)
    {
        if (log)
            log->Printf ("%p ConnectionFileDescriptor::ConnectionFileDescriptor () - could not make pipe: %s",
                         this,
                         strerror(errno));
    }
    else
    {
        m_pipe_read  = filedes[0];
        m_pipe_write = filedes[1];
    }
}

void
ConnectionFileDescriptor::CloseCommandPipe ()
{
    if (m_pipe_read != -1)
    {
#ifdef _POSIX_SOURCE	
        close (m_pipe_read);
#endif
        m_pipe_read = -1;
    }
    
    if (m_pipe_write != -1)
    {
#ifdef _POSIX_SOURCE	
        close (m_pipe_write);
#endif
        m_pipe_write = -1;
    }
}

bool
ConnectionFileDescriptor::IsConnected () const
{
    return m_fd_send >= 0 || m_fd_recv >= 0;
}

ConnectionStatus
ConnectionFileDescriptor::Connect (const char *s, Error *error_ptr)
{
    Mutex::Locker locker (m_mutex);
    LogSP log(lldb_private::GetLogIfAnyCategoriesSet (LIBLLDB_LOG_CONNECTION));
    if (log)
        log->Printf ("%p ConnectionFileDescriptor::Connect (url = '%s')", this, s);

    OpenCommandPipe();
    
    if (s && s[0])
    {
        char *end = NULL;
        if (strstr(s, "listen://"))
        {
            // listen://HOST:PORT
            unsigned long listen_port = ::strtoul(s + strlen("listen://"), &end, 0);
            return SocketListen (listen_port, error_ptr);
        }
        else if (strstr(s, "unix-accept://"))
        {
            // unix://SOCKNAME
            return NamedSocketAccept (s + strlen("unix-accept://"), error_ptr);
        }
        else if (strstr(s, "connect://"))
        {
            return ConnectTCP (s + strlen("connect://"), error_ptr);
        }
        else if (strstr(s, "tcp-connect://"))
        {
            return ConnectTCP (s + strlen("tcp-connect://"), error_ptr);
        }
        else if (strstr(s, "udp://"))
        {
            return ConnectUDP (s + strlen("udp://"), error_ptr);
        }
        else if (strstr(s, "fd://"))
        {
            // Just passing a native file descriptor within this current process
            // that is already opened (possibly from a service or other source).
            s += strlen ("fd://");
            bool success = false;
            m_fd_send = m_fd_recv = Args::StringToSInt32 (s, -1, 0, &success);
            
            if (success)
            {
                // We have what looks to be a valid file descriptor, but we 
                // should make sure it is. We currently are doing this by trying to
                // get the flags from the file descriptor and making sure it 
                // isn't a bad fd.
                errno = 0;
#ifdef _POSIX_SOURCE
                 int flags = ::fcntl (m_fd_send, F_GETFL, 0);
#else
                int flags = -1;
#endif
                if (flags == -1 || errno == EBADF)
                {
                    if (error_ptr)
                        error_ptr->SetErrorStringWithFormat ("stale file descriptor: %s", s);
                    m_fd_send = m_fd_recv = -1;
                    return eConnectionStatusError;
                }
                else
                {
                    // Try and get a socket option from this file descriptor to 
                    // see if this is a socket and set m_is_socket accordingly.
                    int resuse;
                    bool is_socket = GetSocketOption (m_fd_send, SOL_SOCKET, SO_REUSEADDR, resuse) == 0;
                    if (is_socket)
                        m_fd_send_type = m_fd_recv_type = eFDTypeSocket;
                    // Don't take ownership of a file descriptor that gets passed
                    // to us since someone else opened the file descriptor and
                    // handed it to us. 
                    // TODO: Since are using a URL to open connection we should 
                    // eventually parse options using the web standard where we
                    // have "fd://123?opt1=value;opt2=value" and we can have an
                    // option be "owns=1" or "owns=0" or something like this to
                    // allow us to specify this. For now, we assume we must 
                    // assume we don't own it.
                    m_should_close_fd = false;
                    return eConnectionStatusSuccess;
                }
            }
            
            if (error_ptr)
                error_ptr->SetErrorStringWithFormat ("invalid file descriptor: \"fd://%s\"", s);
            m_fd_send = m_fd_recv = -1;
            return eConnectionStatusError;
        }
        else if (strstr(s, "file://"))
        {
            // file:///PATH
            const char *path = s + strlen("file://");
            do
            {
#ifdef _POSIX_SOURCE
                m_fd_send = m_fd_recv = ::open (path, O_RDWR);
#endif				
            } while (m_fd_send == -1 && errno == EINTR);
            if (m_fd_send == -1)
            {
                if (error_ptr)
                    error_ptr->SetErrorToErrno();
                return eConnectionStatusError;
            }
#ifdef _POSIX_SOURCE
            int flags = ::fcntl (m_fd_send, F_GETFL, 0);
#else
			int flags = -1;
#endif			
            if (flags >= 0)
            {
#ifdef _POSIX_SOURCE			
                if ((flags & O_NONBLOCK) == 0)
                {
                    flags |= O_NONBLOCK;
                    ::fcntl (m_fd_send, F_SETFL, flags);
                }
#endif				
            }
            m_should_close_fd = true;
            return eConnectionStatusSuccess;
        }
        if (error_ptr)
            error_ptr->SetErrorStringWithFormat ("unsupported connection URL: '%s'", s);
        return eConnectionStatusError;
    }
    if (error_ptr)
        error_ptr->SetErrorString("invalid connect arguments");
    return eConnectionStatusError;
}

ConnectionStatus
ConnectionFileDescriptor::Disconnect (Error *error_ptr)
{
    LogSP log(lldb_private::GetLogIfAnyCategoriesSet (LIBLLDB_LOG_CONNECTION));
    if (log)
        log->Printf ("%p ConnectionFileDescriptor::Disconnect ()", this);

    ConnectionStatus status = eConnectionStatusSuccess;

    if (m_fd_send < 0 && m_fd_recv < 0)
    {
        if (log)
            log->Printf ("%p ConnectionFileDescriptor::Disconnect(): Nothing to disconnect", this);
        return eConnectionStatusSuccess;
    }
    
    // Try to get the ConnectionFileDescriptor's mutex.  If we fail, that is quite likely
    // because somebody is doing a blocking read on our file descriptor.  If that's the case,
    // then send the "q" char to the command file channel so the read will wake up and the connection
    // will then know to shut down.
    
    m_shutting_down = true;
    
    Mutex::Locker locker;
    bool got_lock= locker.TryLock (m_mutex);
    
    if (!got_lock)
    {
        if (m_pipe_write != -1 )
        {
#ifdef _POSIX_SOURCE
            write (m_pipe_write, "q", 1);
            close (m_pipe_write);
#endif
            m_pipe_write = -1;
        }
        locker.Lock (m_mutex);
    }
    
    if (m_should_close_fd == true)
    {
        if (m_fd_send == m_fd_recv)
        {
            status = Close (m_fd_send, error_ptr);
        }
        else
        {
            // File descriptors are the different, close both if needed
            if (m_fd_send >= 0)
                status = Close (m_fd_send, error_ptr);
            if (m_fd_recv >= 0)
            {
                ConnectionStatus recv_status = Close (m_fd_recv, error_ptr);
                if (status == eConnectionStatusSuccess)
                    status = recv_status;
            }
        }
    }

    // Now set all our descriptors to invalid values.
    
    m_fd_send = m_fd_recv = -1;

    if (status != eConnectionStatusSuccess)
    {
        
        return status;
    }
        
    m_shutting_down = false;
    return eConnectionStatusSuccess;
}

size_t
ConnectionFileDescriptor::Read (void *dst, 
                                size_t dst_len, 
                                uint32_t timeout_usec,
                                ConnectionStatus &status, 
                                Error *error_ptr)
{
    LogSP log(lldb_private::GetLogIfAnyCategoriesSet (LIBLLDB_LOG_CONNECTION));
    if (log)
        log->Printf ("%p ConnectionFileDescriptor::Read () ::read (fd = %i, dst = %p, dst_len = %zu)...",
                     this, m_fd_recv, dst, dst_len);

    Mutex::Locker locker;
    bool got_lock = locker.TryLock (m_mutex);
    if (!got_lock)
    {
        if (log)
            log->Printf ("%p ConnectionFileDescriptor::Read () failed to get the connection lock.",
                     this);
        if (error_ptr)
            error_ptr->SetErrorString ("failed to get the connection lock for read.");
            
        status = eConnectionStatusTimedOut;
        return 0;
    }
    else if (m_shutting_down)
        return eConnectionStatusError;
    
    ssize_t bytes_read = 0;

    status = BytesAvailable (timeout_usec, error_ptr);
    if (status == eConnectionStatusSuccess)
    {
        do
        {
#ifdef _POSIX_SOURCE
            bytes_read = ::read (m_fd_recv, dst, dst_len);
#endif			
        } while (bytes_read < 0 && errno == EINTR);
    }

    if (status != eConnectionStatusSuccess)
        return 0;

    Error error;
    if (bytes_read == 0)
    {
        error.Clear(); // End-of-file.  Do not automatically close; pass along for the end-of-file handlers.
        status = eConnectionStatusEndOfFile;
    }
    else if (bytes_read < 0)
    {
        error.SetErrorToErrno();
    }
    else
    {
        error.Clear();
    }

    if (log)
        log->Printf ("%p ConnectionFileDescriptor::Read () ::read (fd = %i, dst = %p, dst_len = %zu) => %zi, error = %s",
                     this, 
                     m_fd_recv, 
                     dst, 
                     dst_len, 
                     bytes_read, 
                     error.AsCString());

    if (error_ptr)
        *error_ptr = error;

    if (error.Fail())
    {
        uint32_t error_value = error.GetError();
        switch (error_value)
        {
        case EAGAIN:    // The file was marked for non-blocking I/O, and no data were ready to be read.
            if (m_fd_recv_type == eFDTypeSocket || m_fd_recv_type == eFDTypeSocketUDP)
                status = eConnectionStatusTimedOut;
            else
                status = eConnectionStatusSuccess;
            return 0;

        case EFAULT:    // Buf points outside the allocated address space.
        case EINTR:     // A read from a slow device was interrupted before any data arrived by the delivery of a signal.
        case EINVAL:    // The pointer associated with fildes was negative.
        case EIO:       // An I/O error occurred while reading from the file system.
                        // The process group is orphaned.
                        // The file is a regular file, nbyte is greater than 0,
                        // the starting position is before the end-of-file, and
                        // the starting position is greater than or equal to the
                        // offset maximum established for the open file
                        // descriptor associated with fildes.
        case EISDIR:    // An attempt is made to read a directory.
        case ENOBUFS:   // An attempt to allocate a memory buffer fails.
        case ENOMEM:    // Insufficient memory is available.
            status = eConnectionStatusError;
            break;  // Break to close....

        case ENOENT:    // no such file or directory
        case EBADF:     // fildes is not a valid file or socket descriptor open for reading.
        case ENXIO:     // An action is requested of a device that does not exist..
                        // A requested action cannot be performed by the device.
        case ECONNRESET:// The connection is closed by the peer during a read attempt on a socket.
        case ENOTCONN:  // A read is attempted on an unconnected socket.
            status = eConnectionStatusLostConnection;
            break;  // Break to close....

        case ETIMEDOUT: // A transmission timeout occurs during a read attempt on a socket.
            status = eConnectionStatusTimedOut;
            return 0;
        }

        //Disconnect (NULL);
        return 0;
    }
    return bytes_read;
}

size_t
ConnectionFileDescriptor::Write (const void *src, size_t src_len, ConnectionStatus &status, Error *error_ptr)
{
    LogSP log(lldb_private::GetLogIfAnyCategoriesSet (LIBLLDB_LOG_CONNECTION));
    if (log)
        log->Printf ("%p ConnectionFileDescriptor::Write (src = %p, src_len = %zu)", this, src, src_len);

    if (!IsConnected ())
    {
        if (error_ptr)
            error_ptr->SetErrorString("not connected");
        status = eConnectionStatusNoConnection;
        return 0;
    }


    Error error;

    ssize_t bytes_sent = 0;

#ifdef _POSIX_SOURCE
    switch (m_fd_send_type)
    {
        case eFDTypeFile:       // Other FD requireing read/write
            do
            {
                bytes_sent = ::write (m_fd_send, src, src_len);
            } while (bytes_sent < 0 && errno == EINTR);
            break;
            
        case eFDTypeSocket:     // Socket requiring send/recv
            do
            {
                bytes_sent = ::send (m_fd_send, src, src_len, 0);
            } while (bytes_sent < 0 && errno == EINTR);
            break;
            
        case eFDTypeSocketUDP:  // Unconnected UDP socket requiring sendto/recvfrom
            assert (m_udp_send_sockaddr.GetFamily() != 0);
            do
            {
                bytes_sent = ::sendto (m_fd_send, 
                                       src, 
                                       src_len, 
                                       0, 
                                       m_udp_send_sockaddr, 
                                       m_udp_send_sockaddr.GetLength());
            } while (bytes_sent < 0 && errno == EINTR);
            break;
    }
#endif	

    if (bytes_sent < 0)
        error.SetErrorToErrno ();
    else
        error.Clear ();

    if (log)
    {
        switch (m_fd_send_type)
        {
            case eFDTypeFile:       // Other FD requireing read/write
                log->Printf ("%p ConnectionFileDescriptor::Write()  ::write (fd = %i, src = %p, src_len = %zu) => %zi (error = %s)",
                             this, 
                             m_fd_send, 
                             src, 
                             src_len, 
                             bytes_sent, 
                             error.AsCString());
                break;
                
            case eFDTypeSocket:     // Socket requiring send/recv
                log->Printf ("%p ConnectionFileDescriptor::Write()  ::send (socket = %i, src = %p, src_len = %zu, flags = 0) => %zi (error = %s)",
                             this, 
                             m_fd_send, 
                             src, 
                             src_len, 
                             bytes_sent, 
                             error.AsCString());
                break;
                
            case eFDTypeSocketUDP:  // Unconnected UDP socket requiring sendto/recvfrom
                log->Printf ("%p ConnectionFileDescriptor::Write()  ::sendto (socket = %i, src = %p, src_len = %zu, flags = 0) => %zi (error = %s)",
                             this, 
                             m_fd_send, 
                             src, 
                             src_len, 
                             bytes_sent, 
                             error.AsCString());
                break;
        }
    }

    if (error_ptr)
        *error_ptr = error;

    if (error.Fail())
    {
        switch (error.GetError())
        {
        case EAGAIN:
        case EINTR:
            status = eConnectionStatusSuccess;
            return 0;

        case ECONNRESET:// The connection is closed by the peer during a read attempt on a socket.
        case ENOTCONN:  // A read is attempted on an unconnected socket.
            status = eConnectionStatusLostConnection;
            break;  // Break to close....

        default:
            status = eConnectionStatusError;
            break;  // Break to close....
        }

        return 0;
    }

    status = eConnectionStatusSuccess;
    return bytes_sent;
}

ConnectionStatus
ConnectionFileDescriptor::BytesAvailable (uint32_t timeout_usec, Error *error_ptr)
{
    // Don't need to take the mutex here separately since we are only called from Read.  If we
    // ever get used more generally we will need to lock here as well.
    
    LogSP log(lldb_private::GetLogIfAnyCategoriesSet (LIBLLDB_LOG_CONNECTION));
    if (log)
        log->Printf("%p ConnectionFileDescriptor::BytesAvailable (timeout_usec = %u)", this, timeout_usec);
    struct timeval *tv_ptr;
    struct timeval tv;
    if (timeout_usec == UINT32_MAX)
    {
        // Infinite wait...
        tv_ptr = NULL;
    }
    else
    {
        TimeValue time_value;
        time_value.OffsetWithMicroSeconds (timeout_usec);
        tv = time_value.GetAsTimeVal();
        tv_ptr = &tv;
    }

    while (m_fd_recv >= 0)
    {
        fd_set read_fds;
        FD_ZERO (&read_fds);
        FD_SET (m_fd_recv, &read_fds);
        if (m_pipe_read != -1)
            FD_SET (m_pipe_read, &read_fds);
        int nfds = std::max<int>(m_fd_recv, m_pipe_read) + 1;
        
        Error error;


        if (log)
            log->Printf("%p ConnectionFileDescriptor::BytesAvailable()  ::select (nfds = %i, fd = %i, NULL, NULL, timeout = %p)...",
                        this, nfds, m_fd_recv, tv_ptr);

        const int num_set_fds = ::select (nfds, &read_fds, NULL, NULL, tv_ptr);
        if (num_set_fds < 0)
            error.SetErrorToErrno();
        else
            error.Clear();

        if (log)
            log->Printf("%p ConnectionFileDescriptor::BytesAvailable()  ::select (nfds = %i, fd = %i, NULL, NULL, timeout = %p) => %d, error = %s",
                        this, nfds, m_fd_recv, tv_ptr, num_set_fds, error.AsCString());

        if (error_ptr)
            *error_ptr = error;

        if (error.Fail())
        {
            switch (error.GetError())
            {
            case EBADF:     // One of the descriptor sets specified an invalid descriptor.
                return eConnectionStatusLostConnection;

            case EINVAL:    // The specified time limit is invalid. One of its components is negative or too large.
            default:        // Other unknown error
                return eConnectionStatusError;

            case EAGAIN:    // The kernel was (perhaps temporarily) unable to
                            // allocate the requested number of file descriptors,
                            // or we have non-blocking IO
            case EINTR:     // A signal was delivered before the time limit
                            // expired and before any of the selected events
                            // occurred.
                break;      // Lets keep reading to until we timeout
            }
        }
        else if (num_set_fds == 0)
        {
            return eConnectionStatusTimedOut;
        }
        else if (num_set_fds > 0)
        {
            if (m_pipe_read != -1 && FD_ISSET(m_pipe_read, &read_fds))
            {
                // We got a command to exit.  Read the data from that pipe:
                char buffer[16];
                ssize_t bytes_read;
                
                do
                {
#ifdef _POSIX_SOURCE
                    bytes_read = ::read (m_pipe_read, buffer, sizeof(buffer));
#endif					
                } while (bytes_read < 0 && errno == EINTR);
                assert (bytes_read == 1 && buffer[0] == 'q');
                
                if (log)
                    log->Printf("%p ConnectionFileDescriptor::BytesAvailable() got data: %*s from the command channel.",
                                this, (int) bytes_read, buffer);

                return eConnectionStatusEndOfFile;
            }
            else
                return eConnectionStatusSuccess;
        }
    }

    if (error_ptr)
        error_ptr->SetErrorString("not connected");
    return eConnectionStatusLostConnection;
}

ConnectionStatus
ConnectionFileDescriptor::Close (int& fd, Error *error_ptr)
{
    if (error_ptr)
        error_ptr->Clear();
    bool success = true;
    // Avoid taking a lock if we can
    if (fd >= 0)
    {
        Mutex::Locker locker (m_mutex);
        // Check the FD after the lock is taken to ensure only one thread
        // can get into the close scope below
        if (fd >= 0)
        {
            LogSP log(lldb_private::GetLogIfAnyCategoriesSet (LIBLLDB_LOG_CONNECTION));
            if (log)
                log->Printf ("%p ConnectionFileDescriptor::Close (fd = %i)", this,fd);

#ifdef _POSIX_SOURCE
            success = ::close (fd) == 0;
#endif			
            // A reference to a FD was passed in, set it to an invalid value
            fd = -1;
            if (!success && error_ptr)
            {
                // Only set the error if we have been asked to since something else
                // might have caused us to try and shut down the connection and may
                // have already set the error.
                error_ptr->SetErrorToErrno();
            }
        }
    }
    if (success)
        return eConnectionStatusSuccess;
    else
        return eConnectionStatusError;
}

ConnectionStatus
ConnectionFileDescriptor::NamedSocketAccept (const char *socket_name, Error *error_ptr)
{
#ifdef _POSIX_SOURCE
    ConnectionStatus result = eConnectionStatusError;
    struct sockaddr_un saddr_un;

    m_fd_send_type = m_fd_recv_type = eFDTypeSocket;
    
    int listen_socket = ::socket (AF_UNIX, SOCK_STREAM, 0);
    if (listen_socket == -1)
    {
        if (error_ptr)
            error_ptr->SetErrorToErrno();
        return eConnectionStatusError;
    }

    saddr_un.sun_family = AF_UNIX;
    ::strncpy(saddr_un.sun_path, socket_name, sizeof(saddr_un.sun_path) - 1);
    saddr_un.sun_path[sizeof(saddr_un.sun_path) - 1] = '\0';
#if defined(__APPLE__) || defined(__FreeBSD__)
    saddr_un.sun_len = SUN_LEN (&saddr_un);
#endif

    if (::bind (listen_socket, (struct sockaddr *)&saddr_un, SUN_LEN (&saddr_un)) == 0) 
    {
        if (::listen (listen_socket, 5) == 0) 
        {
            m_fd_send = m_fd_recv = ::accept (listen_socket, NULL, 0);
            if (m_fd_send > 0)
            {
                m_should_close_fd = true;

                if (error_ptr)
                    error_ptr->Clear();
                result = eConnectionStatusSuccess;
            }
        }
    }
    
    if (result != eConnectionStatusSuccess)
    {
        if (error_ptr)
            error_ptr->SetErrorToErrno();
    }
    // We are done with the listen port
    Close (listen_socket, NULL);
    return result;
#else
    return eConnectionStatusError;
#endif	
}

ConnectionStatus
ConnectionFileDescriptor::NamedSocketConnect (const char *socket_name, Error *error_ptr)
{
#ifdef _POSIX_SOURCE
    Disconnect (NULL);
    m_fd_send_type = m_fd_recv_type = eFDTypeSocket;

    // Open the socket that was passed in as an option
    struct sockaddr_un saddr_un;
    m_fd_send = m_fd_recv = ::socket (AF_UNIX, SOCK_STREAM, 0);
    if (m_fd_send == -1)
    {
        if (error_ptr)
            error_ptr->SetErrorToErrno();
        return eConnectionStatusError;
    }

    saddr_un.sun_family = AF_UNIX;
    ::strncpy(saddr_un.sun_path, socket_name, sizeof(saddr_un.sun_path) - 1);
    saddr_un.sun_path[sizeof(saddr_un.sun_path) - 1] = '\0';
#if defined(__APPLE__) || defined(__FreeBSD__)
    saddr_un.sun_len = SUN_LEN (&saddr_un);
#endif

    if (::connect (m_fd_send, (struct sockaddr *)&saddr_un, SUN_LEN (&saddr_un)) < 0) 
    {
        if (error_ptr)
            error_ptr->SetErrorToErrno();
        Disconnect (NULL);
        return eConnectionStatusError;
    }
    if (error_ptr)
        error_ptr->Clear();
    return eConnectionStatusSuccess;
#else
    return eConnectionStatusError;
#endif
}

ConnectionStatus
ConnectionFileDescriptor::SocketListen (uint16_t listen_port_num, Error *error_ptr)
{
#if _POSIX_SOURCE
    LogSP log(lldb_private::GetLogIfAnyCategoriesSet (LIBLLDB_LOG_CONNECTION));
    if (log)
        log->Printf ("%p ConnectionFileDescriptor::SocketListen (port = %i)", this, listen_port_num);

    Disconnect (NULL);
    m_fd_send_type = m_fd_recv_type = eFDTypeSocket;
    int listen_port = ::socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_port == -1)
    {
        if (error_ptr)
            error_ptr->SetErrorToErrno();
        return eConnectionStatusError;
    }

    // enable local address reuse
    SetSocketOption (listen_port, SOL_SOCKET, SO_REUSEADDR, 1);

    SocketAddress localhost;
    if (localhost.SetToLocalhost (AF_INET, listen_port_num))
    {
        int err = ::bind (listen_port, localhost, localhost.GetLength());
        if (err == -1)
        {
            if (error_ptr)
                error_ptr->SetErrorToErrno();
            Close (listen_port, NULL);
            return eConnectionStatusError;
        }

        err = ::listen (listen_port, 1);
        if (err == -1)
        {
            if (error_ptr)
                error_ptr->SetErrorToErrno();
            Close (listen_port, NULL);
            return eConnectionStatusError;
        }

        m_fd_send = m_fd_recv = ::accept (listen_port, NULL, 0);
        if (m_fd_send == -1)
        {
            if (error_ptr)
                error_ptr->SetErrorToErrno();
            Close (listen_port, NULL);
            return eConnectionStatusError;
        }
    }

    // We are done with the listen port
    Close (listen_port, NULL);

    m_should_close_fd = true;

    // Keep our TCP packets coming without any delays.
    SetSocketOption (m_fd_send, IPPROTO_TCP, TCP_NODELAY, 1);
    if (error_ptr)
        error_ptr->Clear();
    return eConnectionStatusSuccess;
#else
    return eConnectionStatusError;
#endif
}

ConnectionStatus
ConnectionFileDescriptor::ConnectTCP (const char *host_and_port, Error *error_ptr)
{
    LogSP log(lldb_private::GetLogIfAnyCategoriesSet (LIBLLDB_LOG_CONNECTION));
    if (log)
        log->Printf ("%p ConnectionFileDescriptor::ConnectTCP (host/port = %s)", this, host_and_port);
    Disconnect (NULL);

    m_fd_send_type = m_fd_recv_type = eFDTypeSocket;
    std::string host_str;
    std::string port_str;
    int32_t port = INT32_MIN;
    if (!DecodeHostAndPort (host_and_port, host_str, port_str, port, error_ptr))
        return eConnectionStatusError;

    // Create the socket
    m_fd_send = m_fd_recv = ::socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_fd_send == -1)
    {
        if (error_ptr)
            error_ptr->SetErrorToErrno();
        return eConnectionStatusError;
    }

    m_should_close_fd = true;

    // Enable local address reuse
    SetSocketOption (m_fd_send, SOL_SOCKET, SO_REUSEADDR, 1);

    struct sockaddr_in sa;
    ::memset (&sa, 0, sizeof (sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons (port);

    int inet_pton_result = ::inet_pton (AF_INET, host_str.c_str(), &sa.sin_addr);

    if (inet_pton_result <= 0)
    {
        struct hostent *host_entry = gethostbyname (host_str.c_str());
        if (host_entry)
            host_str = ::inet_ntoa (*(struct in_addr *)*host_entry->h_addr_list);
        inet_pton_result = ::inet_pton (AF_INET, host_str.c_str(), &sa.sin_addr);
        if (inet_pton_result <= 0)
        {

            if (error_ptr)
            {
                if (inet_pton_result == -1)
                    error_ptr->SetErrorToErrno();
                else
                    error_ptr->SetErrorStringWithFormat("invalid host string: '%s'", host_str.c_str());
            }
            Disconnect (NULL);

            return eConnectionStatusError;
        }
    }

    if (-1 == ::connect (m_fd_send, (const struct sockaddr *)&sa, sizeof(sa)))
    {
        if (error_ptr)
            error_ptr->SetErrorToErrno();
        Disconnect (NULL);

        return eConnectionStatusError;
    }

    // Keep our TCP packets coming without any delays.
    SetSocketOption (m_fd_send, IPPROTO_TCP, TCP_NODELAY, 1);
    if (error_ptr)
        error_ptr->Clear();
    return eConnectionStatusSuccess;
}

ConnectionStatus
ConnectionFileDescriptor::ConnectUDP (const char *host_and_port, Error *error_ptr)
{
    LogSP log(lldb_private::GetLogIfAnyCategoriesSet (LIBLLDB_LOG_CONNECTION));
    if (log)
        log->Printf ("%p ConnectionFileDescriptor::ConnectUDP (host/port = %s)", this, host_and_port);
    Disconnect (NULL);

    m_fd_send_type = m_fd_recv_type = eFDTypeSocketUDP;
    
    std::string host_str;
    std::string port_str;
    int32_t port = INT32_MIN;
    if (!DecodeHostAndPort (host_and_port, host_str, port_str, port, error_ptr))
        return eConnectionStatusError;

    // Setup the receiving end of the UDP connection on this localhost
    // on port zero. After we bind to port zero we can read the port.
    m_fd_recv = ::socket (AF_INET, SOCK_DGRAM, 0);
    if (m_fd_recv == -1)
    {
        // Socket creation failed...
        if (error_ptr)
            error_ptr->SetErrorToErrno();
    }
    else
    {
        // Socket was created, now lets bind to the requested port
        SocketAddress addr;
        addr.SetToLocalhost (AF_INET, 0);

        if (::bind (m_fd_recv, addr, addr.GetLength()) == -1)
        {
            // Bind failed...
            if (error_ptr)
                error_ptr->SetErrorToErrno();
            Disconnect (NULL);
        }
    }

    if (m_fd_recv == -1)
        return eConnectionStatusError;

    // At this point we have setup the recieve port, now we need to 
    // setup the UDP send socket
   
    struct addrinfo hints;
    struct addrinfo *service_info_list = NULL;
    
    ::memset (&hints, 0, sizeof(hints)); 
    hints.ai_family = AF_INET; 
    hints.ai_socktype = SOCK_DGRAM;
    int err = ::getaddrinfo (host_str.c_str(), port_str.c_str(), &hints, &service_info_list);
    if (err != 0)
    {
        if (error_ptr)
            error_ptr->SetErrorStringWithFormat("getaddrinfo(%s, %s, &hints, &info) returned error %i (%s)", 
                                                host_str.c_str(), 
                                                port_str.c_str(),
                                                err,
                                                gai_strerror(err));
        Disconnect (NULL);
        return eConnectionStatusError;        
    }
    
    for (struct addrinfo *service_info_ptr = service_info_list; 
         service_info_ptr != NULL; 
         service_info_ptr = service_info_ptr->ai_next) 
    {
        m_fd_send = ::socket (service_info_ptr->ai_family, 
                              service_info_ptr->ai_socktype,
                              service_info_ptr->ai_protocol);
        
        if (m_fd_send != -1)
        {
            m_udp_send_sockaddr = service_info_ptr;
            break;
        }
        else
            continue;
    }
    
    :: freeaddrinfo (service_info_list);

    if (m_fd_send == -1)
    {
        Disconnect (NULL);
        return eConnectionStatusError;
    }

    if (error_ptr)
        error_ptr->Clear();

    m_should_close_fd = true;
    return eConnectionStatusSuccess;
}

#if defined(__MINGW32__) || defined(__MINGW64__) || defined(_MSC_VER)
typedef const char * set_socket_option_arg_type;
typedef char * get_socket_option_arg_type;
#else // #if defined(__MINGW32__) || defined(__MINGW64__)
typedef const void * set_socket_option_arg_type;
typedef void * get_socket_option_arg_type;
#endif // #if defined(__MINGW32__) || defined(__MINGW64__)

int
ConnectionFileDescriptor::GetSocketOption(int fd, int level, int option_name, int &option_value)
{
    get_socket_option_arg_type option_value_p = reinterpret_cast<get_socket_option_arg_type>(&option_value);
    socklen_t option_value_size = sizeof(int);
	return ::getsockopt(fd, level, option_name, option_value_p, &option_value_size);
}

int
ConnectionFileDescriptor::SetSocketOption(int fd, int level, int option_name, int option_value)
{
    set_socket_option_arg_type option_value_p = reinterpret_cast<get_socket_option_arg_type>(&option_value);
	return ::setsockopt(fd, level, option_name, option_value_p, sizeof(option_value));
}

#ifdef _WIN32
static unsigned long int tv2ms(struct timeval *a)
{
    return ((a->tv_sec * 1000) + (a->tv_usec / 1000));
}
#endif

bool
ConnectionFileDescriptor::SetSocketReceiveTimeout (uint32_t timeout_usec)
{
    switch (m_fd_recv_type)
    {
        case eFDTypeFile:       // Other FD requireing read/write
            break;
            
        case eFDTypeSocket:     // Socket requiring send/recv
        case eFDTypeSocketUDP:  // Unconnected UDP socket requiring sendto/recvfrom
        {
            // Check in case timeout for m_fd has already been set to this value
            if (timeout_usec == m_socket_timeout_usec)
                return true;
            //printf ("ConnectionFileDescriptor::SetSocketReceiveTimeout (timeout_usec = %u)\n", timeout_usec);

            struct timeval timeout;
            if (timeout_usec == UINT32_MAX)
            {
                timeout.tv_sec = 0;
                timeout.tv_usec = 0;
            }
            else if (timeout_usec == 0)
            {
                // Sending in zero does an infinite timeout, so set this as low
                // as we can go to get an effective zero timeout...
                timeout.tv_sec = 0;
                timeout.tv_usec = 1;
            }
            else
            {
                timeout.tv_sec = timeout_usec / TimeValue::MicroSecPerSec;
                timeout.tv_usec = timeout_usec % TimeValue::MicroSecPerSec;
            }
#ifndef _WIN32
            const void* timeopt = &timeout;
#else
            DWORD ms = tv2ms(&timeout);
            const char* timeopt = reinterpret_cast<const char*>(ms);
#endif
            if (::setsockopt (m_fd_recv, SOL_SOCKET, SO_RCVTIMEO, timeopt, sizeof(timeopt)) == 0)
            {
                m_socket_timeout_usec = timeout_usec;
                return true;
            }
        }
    }
    return false;
}

in_port_t
ConnectionFileDescriptor::GetSocketPort (int fd)
{
    // We bound to port zero, so we need to figure out which port we actually bound to
    SocketAddress sock_addr;
    socklen_t sock_addr_len = sock_addr.GetMaxLength ();
    if (::getsockname (fd, sock_addr, &sock_addr_len) == 0)
        return sock_addr.GetPort ();

    return 0;
}

// If the read file descriptor is a socket, then return
// the port number that is being used by the socket.
in_port_t
ConnectionFileDescriptor::GetReadPort () const
{
    return ConnectionFileDescriptor::GetSocketPort (m_fd_recv);
}

// If the write file descriptor is a socket, then return
// the port number that is being used by the socket.
in_port_t
ConnectionFileDescriptor::GetWritePort () const
{
    return ConnectionFileDescriptor::GetSocketPort (m_fd_send);
}


