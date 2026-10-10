// Minimal AF_UNIX SOCK_SEQPACKET helpers with fd passing.
#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

namespace vrl {

// Socket name from VRLAYER_SOCKET or the default. Names starting with '/' are
// filesystem paths, anything else lives in the abstract namespace (which is
// visible inside the Steam Linux Runtime container).
std::string socket_name();

// Non-blocking listening socket. Returns fd or -1.
int ipc_listen(const std::string& name);
// Accept a pending connection (non-blocking). Returns fd or -1.
int ipc_accept(int listen_fd);
// Connect (blocking connect, then switched to non-blocking). Returns fd or -1.
int ipc_connect(const std::string& name);

// Send one packet, optionally with file descriptors. Never raises SIGPIPE.
bool ipc_send(int fd, const void* data, size_t size, const int* fds = nullptr, int nfds = 0);

enum : ssize_t { kIpcWouldBlock = -2, kIpcClosed = 0, kIpcError = -1 };
// Receive one packet without blocking. Received fds are appended to *fds (or
// closed when fds is null). Returns bytes read, kIpcWouldBlock, kIpcClosed or
// kIpcError.
ssize_t ipc_recv(int fd, void* buf, size_t cap, std::vector<int>* fds);
// Same but waits up to timeout_ms for data.
ssize_t ipc_recv_timeout(int fd, void* buf, size_t cap, std::vector<int>* fds, int timeout_ms);

void close_fds(std::vector<int>& fds);

}  // namespace vrl
