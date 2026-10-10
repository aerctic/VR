#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

#include "protocol.h"

namespace vrl {

std::string socket_name() {
    const char* env = getenv("VRLAYER_SOCKET");
    if (env && *env) return env;
    return kDefaultSocketName;
}

static socklen_t make_addr(const std::string& name, sockaddr_un* addr) {
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    size_t max = sizeof(addr->sun_path) - 1;
    if (!name.empty() && name[0] == '/') {
        strncpy(addr->sun_path, name.c_str(), max);
        return sizeof(sockaddr_un);
    }
    // Abstract namespace: leading NUL, no terminator, exact length.
    size_t n = name.size() < max ? name.size() : max;
    memcpy(addr->sun_path + 1, name.data(), n);
    return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + n);
}

int ipc_listen(const std::string& name) {
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_un addr;
    socklen_t len = make_addr(name, &addr);
    if (name[0] == '/') unlink(name.c_str());
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), len) < 0 || listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int ipc_accept(int listen_fd) {
    return accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
}

int ipc_connect(const std::string& name) {
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_un addr;
    socklen_t len = make_addr(name, &addr);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), len) < 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

bool ipc_send(int fd, const void* data, size_t size, const int* fds, int nfds) {
    iovec iov{const_cast<void*>(data), size};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    alignas(cmsghdr) char ctrl[CMSG_SPACE(sizeof(int) * 2 * kMaxSlots)];
    if (nfds > 0) {
        if (nfds > static_cast<int>(2 * kMaxSlots)) return false;
        memset(ctrl, 0, sizeof(ctrl));
        msg.msg_control = ctrl;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
        cmsghdr* c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
        memcpy(CMSG_DATA(c), fds, sizeof(int) * nfds);
    }

    for (;;) {
        ssize_t r = sendmsg(fd, &msg, MSG_NOSIGNAL);
        if (r == static_cast<ssize_t>(size)) return true;
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Socket buffer full: wait briefly rather than dropping a control
            // message. Frames are tiny so this practically never happens.
            pollfd p{fd, POLLOUT, 0};
            if (poll(&p, 1, 100) > 0) continue;
        }
        return false;
    }
}

ssize_t ipc_recv(int fd, void* buf, size_t cap, std::vector<int>* fds) {
    iovec iov{buf, cap};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    alignas(cmsghdr) char ctrl[CMSG_SPACE(sizeof(int) * 2 * kMaxSlots)];
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof(ctrl);

    ssize_t r;
    do {
        r = recvmsg(fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    } while (r < 0 && errno == EINTR);
    if (r < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? kIpcWouldBlock : kIpcError;

    for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        size_t n = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        const int* in = reinterpret_cast<const int*>(CMSG_DATA(c));
        for (size_t i = 0; i < n; ++i) {
            if (fds) fds->push_back(in[i]);
            else close(in[i]);
        }
    }
    if (msg.msg_flags & MSG_TRUNC) return kIpcError;
    return r;
}

ssize_t ipc_recv_timeout(int fd, void* buf, size_t cap, std::vector<int>* fds, int timeout_ms) {
    pollfd p{fd, POLLIN, 0};
    int pr;
    do {
        pr = poll(&p, 1, timeout_ms);
    } while (pr < 0 && errno == EINTR);
    if (pr == 0) return kIpcWouldBlock;
    if (pr < 0) return kIpcError;
    return ipc_recv(fd, buf, cap, fds);
}

void close_fds(std::vector<int>& fds) {
    for (int f : fds)
        if (f >= 0) close(f);
    fds.clear();
}

}  // namespace vrl
