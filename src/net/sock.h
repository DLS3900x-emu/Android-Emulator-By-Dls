// Thin portable layer over Winsock / BSD sockets (BSD side is used by the
// Linux test harness).
#pragma once
#ifdef _WIN32
#ifndef FD_SETSIZE
#define FD_SETSIZE 1024  // must precede winsock2.h; default is 64 sockets
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define AETH_BAD_SOCK INVALID_SOCKET
inline int SockErr() { return WSAGetLastError(); }
inline bool SockWouldBlock(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
inline void SockClose(sock_t s) { closesocket(s); }
inline bool SockNonBlock(sock_t s) {
    u_long one = 1;
    return ioctlsocket(s, FIONBIO, &one) == 0;
}
inline bool SockStartup() {
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0;
}
#define AETH_SHUT_WR SD_SEND
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define AETH_BAD_SOCK (-1)
inline int SockErr() { return errno; }
inline bool SockWouldBlock(int e) { return e == EWOULDBLOCK || e == EAGAIN || e == EINPROGRESS; }
inline void SockClose(sock_t s) { close(s); }
inline bool SockNonBlock(sock_t s) { return fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK) == 0; }
inline bool SockStartup() { return true; }
#define AETH_SHUT_WR SHUT_WR
#endif
