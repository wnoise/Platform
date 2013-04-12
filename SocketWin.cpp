#include "PlatformPch.h"
#include "Platform/Socket.h"
#include "Platform/Assert.h"
#include "Platform/Console.h"

#include <mstcpip.h>

using namespace Helium;

// in milliseconds
#define KEEPALIVE_TIMEOUT 10000
#define KEEPALIVE_INTERVAL 1000

// globals
int32_t g_Count = 0;
WSADATA g_WSAData;

bool Helium::InitializeSockets()
{
    if ( ++g_Count == 1 )
    {
        int result = WSAStartup(MAKEWORD(2,2), &g_WSAData);
        if (result != NO_ERROR)
        {
            Helium::Print(TXT("Socket Support: Error initializing socket layer (%d)\n"), WSAGetLastError());
            return false;
        }
    }

    return true;
}

void Helium::CleanupSockets()
{
    if ( --g_Count == 0 )
    {
        int result = WSACleanup();
        if (result != NO_ERROR)
        {
            Helium::Print(TXT("Socket Support: Error cleaning up socket layer (%d)\n"), WSAGetLastError());
        }
    }
}

void Helium::InitializeSocketThread()
{

}

void Helium::CleanupSocketThread()
{

}

int Helium::GetSocketError()
{
    return WSAGetLastError();
}

Socket::Socket()
: m_Handle( INVALID_SOCKET )
{
    memset(&m_Overlapped, 0, sizeof(m_Overlapped));
    m_Overlapped.hEvent = ::CreateEvent(0, true, false, 0);
    m_TerminateIo = ::CreateEvent(0, true, false, 0);
}

Socket::~Socket()
{
    if ( m_Handle != INVALID_SOCKET )
    {
        Close();
    }

    ::CloseHandle( m_Overlapped.hEvent );
    ::CloseHandle( m_TerminateIo );
}

bool Socket::Create( SocketProtocol protocol )
{
    if (protocol == SocketProtocols::Tcp)
    {
        m_Handle = ::WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    }
    else
    {
        m_Handle = ::WSASocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
    }
    
    m_Protocol = protocol;

    if (m_Handle == INVALID_SOCKET)
    {
        return false;
    }

    // I hate Winsock.  This sets a keepalive timeout so that it can detect when
    // a connection is abnormally terminated (like, say, when you reset your ps3!)
    // Otherwise, it never registers a disconnect.

    if (protocol == SocketProtocols::Tcp)
    {
        struct tcp_keepalive keepalive;
        keepalive.onoff = 1;
        keepalive.keepalivetime = KEEPALIVE_TIMEOUT;
        keepalive.keepaliveinterval = KEEPALIVE_INTERVAL;

        DWORD returned;
        int result = ::WSAIoctl( m_Handle, SIO_KEEPALIVE_VALS, &keepalive, sizeof( keepalive ), NULL, 0, &returned, NULL, NULL );
        if ( result == SOCKET_ERROR )
        {
            Helium::Print( TXT("TCP Support: Error setting keep alive on socket (%d)\n"), WSAGetLastError() );
        }
    }

    return true;
}

bool Socket::Close()
{
    ::SetEvent( m_TerminateIo );
    ::shutdown( m_Handle, SD_BOTH );
    return ::closesocket( m_Handle ) != SOCKET_ERROR;
}

bool Socket::Bind( uint16_t port )
{
    bool reuse = true;
    ::setsockopt( m_Handle, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse) );

    sockaddr_in service;
    service.sin_family = AF_INET;
    service.sin_addr.s_addr = INADDR_ANY;
    service.sin_port = htons(port);
    if ( ::bind( m_Handle, (sockaddr*)&service, sizeof(sockaddr_in) ) == SOCKET_ERROR )
    {
        Helium::Print( TXT("Socket Support: Failed to bind socket (%d)\n"), WSAGetLastError() );
        ::closesocket( m_Handle );
        return false;
    }

    return true;
}

bool Socket::Listen()
{
    HELIUM_ASSERT( m_Protocol == Helium::SocketProtocols::Tcp );
    if ( ::listen( m_Handle, SOMAXCONN) == SOCKET_ERROR )
    {
        Helium::Print( TXT("TCP Support: Failed to listen socket (%d)\n"), WSAGetLastError() );
        ::closesocket( m_Handle );
        return false;
    }

    return true;
}

bool Socket::Connect( uint16_t port, const tchar_t* ip )
{
    HELIUM_ASSERT( m_Protocol == Helium::SocketProtocols::Tcp );

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ip ? inet_addr(ip) : htonl(INADDR_BROADCAST);
    addr.sin_port = htons(port);
    return ::connect( m_Handle, (SOCKADDR*)&addr, sizeof(sockaddr_in)) != SOCKET_ERROR;
}

bool Socket::Accept( Socket& server_socket, sockaddr_in* client_info )
{
    HELIUM_ASSERT( m_Protocol == Helium::SocketProtocols::Tcp );

    int lengthname = sizeof(sockaddr_in);

    m_Protocol = Helium::SocketProtocols::Tcp;
    m_Handle = ::accept( server_socket.m_Handle, (struct sockaddr *)client_info, &lengthname);

    int error = WSAGetLastError();

    return m_Handle != SOCKET_ERROR;
}

bool Socket::Read( void* buffer, uint32_t bytes, uint32_t& read, sockaddr_in* peer )
{
    if (bytes == 0)
    {
        return true;
    }

    WSABUF buf;
    buf.buf = (CHAR*)buffer;
    buf.len = bytes;

    DWORD flags = 0;
    DWORD read_local = 0;
    sockaddr_in addr;
    INT addrSize = sizeof(addr);
    bool udp = m_Protocol == SocketProtocols::Udp;
    int wsa_result = udp ? ::WSARecvFrom(m_Handle, &buf, 1, &read_local, &flags, (SOCKADDR*)(peer ? peer : &addr), &addrSize, &m_Overlapped, NULL) :
                           ::WSARecv    (m_Handle, &buf, 1, &read_local, &flags, &m_Overlapped, NULL);
    if ( wsa_result != 0 )
    {
        if ( WSAGetLastError() != WSA_IO_PENDING )
        {
#ifdef IPC_DEBUG_SOCKETS
            Helium::Print("Socket Support: Failed to initiate overlapped read (%s)\n", Helium::GetErrorString().c_str());
#endif
            return false;
        }
        else
        {
            ::ResetEvent( m_TerminateIo );
            ::ResetEvent( m_Overlapped.hEvent );
            HANDLE events[] = { m_TerminateIo, m_Overlapped.hEvent };
            DWORD result = ::WSAWaitForMultipleEvents(2, events, FALSE, INFINITE, FALSE);

            HELIUM_ASSERT( result != WAIT_FAILED );

            if ( (result - WSA_WAIT_EVENT_0) == 0 )
            {
#ifdef IPC_DEBUG_SOCKETS
                Helium::Print("Socket Support: Terminating read\n");
#endif
                return false;
            }

            if ( !::WSAGetOverlappedResult(m_Handle, &m_Overlapped, &read_local, false, &flags) )
            {
#ifdef IPC_DEBUG_SOCKETS
                Helium::Print("Socket Support: Failed read (%s)\n", Helium::GetErrorString().c_str());
#endif
                return false;
            }
        }
    }

    if (read_local == 0)
    {
        return false;
    }

    read = (uint32_t)read_local;

    return true;
}

bool Socket::Write( void* buffer, uint32_t bytes, uint32_t& wrote, const tchar_t* ip, uint16_t port )
{
    if (bytes == 0)
    {
        return true;
    }

    WSABUF buf;
    buf.buf = (CHAR*)buffer;
    buf.len = bytes;

    DWORD flags = 0;
    DWORD wrote_local = 0;

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = ip ? inet_addr(ip) : htonl(INADDR_BROADCAST);

    bool udp = m_Protocol == SocketProtocols::Udp;
    if (udp)
    {
        bool opt = !ip;
        ::setsockopt( m_Handle, SOL_SOCKET, SO_BROADCAST, (char*)&opt, sizeof(opt) );
    }

    int wsa_result = udp ? ::WSASendTo(m_Handle, &buf, 1, &wrote_local, 0, (SOCKADDR *)&addr, sizeof(sockaddr_in), &m_Overlapped, NULL) :
                           ::WSASend  (m_Handle, &buf, 1, &wrote_local, 0, &m_Overlapped, NULL);
    if ( wsa_result != 0 )
    {
        int last_error = WSAGetLastError();
        if ( WSAGetLastError() != WSA_IO_PENDING )
        {
#ifdef IPC_DEBUG_SOCKETS
            Helium::Print("Socket Support: Failed to initiate overlapped write (%s)\n", Helium::GetErrorString().c_str());
#endif
            return false;
        }
        else
        {
            ::ResetEvent( m_TerminateIo );
            ::ResetEvent( m_Overlapped.hEvent );
            HANDLE events[] = { m_TerminateIo, m_Overlapped.hEvent };
            DWORD result = ::WSAWaitForMultipleEvents(2, events, FALSE, INFINITE, FALSE);

            HELIUM_ASSERT( result != WAIT_FAILED );

            if ( (result - WSA_WAIT_EVENT_0) == 0 )
            {
#ifdef IPC_DEBUG_SOCKETS
                Helium::Print("Socket Support: Terminating write\n");
#endif
                return false;
            }

            if ( !::WSAGetOverlappedResult(m_Handle, &m_Overlapped, &wrote_local, false, &flags) )
            {
#ifdef IPC_DEBUG_SOCKETS
                Helium::Print("Socket Support: Failed write (%s)\n", Helium::GetErrorString().c_str());
#endif
                return false;
            }
        }
    }

    if (wrote_local == 0)
    {
        return false;
    }

    wrote = (uint32_t)wrote_local;

    return true;
}

int Socket::Select( Handle range, fd_set* read_set, fd_set* write_set,struct timeval* timeout )
{
    return ::select( 0, read_set, write_set, 0, timeout);
}
