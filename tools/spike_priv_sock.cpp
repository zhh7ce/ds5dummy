// THROWAWAY probe for the Manager/Worker split. Two unrelated processes — no
// fork, no inheritance, only a listening AF_UNIX socket:
//
//   1. does an O_RDONLY fd let uid 1000 read a root-owned 0600 file it could
//      never open itself?  (authorization happens at open(), not at use)
//   2. does the O_RDWR fd write while the O_RDONLY one fails EBADF?
//      (the access mode travels with the file description, not the caller)
//   3. does an fd keep working after the server unlinked its path?
//      (what crosses the socket is the kernel file object, not a pathname)
//   4. can the same uid 1000 process open() the path itself?  (must be EACCES —
//      this is what decides whether a reopen after USB reconfiguration has to
//      be done by the privileged side forever)
//   5. does one sendmsg carry three fds, and does SO_PEERCRED show the real
//      caller uid on an accepted connection?  (the Manager's auth gate)
//
//   sudo ./build-spike/spike_priv_sock serve    # root: create, open, listen, pass
//        ./build-spike/spike_priv_sock client   # desktop user: receive, use

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

constexpr const char* kSockPath = "/run/ds5-priv-probe.sock";
constexpr const char* kFilePath = "/tmp/ds5-priv-probe-A";  // stays visible
constexpr const char* kGonePath = "/tmp/ds5-priv-probe-C";  // unlinked below
constexpr size_t kMaxFds = 8;

void diep(const char* what) {
    std::fprintf(stderr, "%s: %s\n", what, std::strerror(errno));
    std::exit(1);
}

void fill_addr(sockaddr_un& sa, const char* path) {
    sa.sun_family = AF_UNIX;
    if (std::strlen(path) >= sizeof(sa.sun_path)) {
        std::fprintf(stderr, "socket path too long: %s\n", path);
        std::exit(1);
    }
    std::memcpy(sa.sun_path, path, std::strlen(path) + 1);
}

// One message, N descriptors: the Manager hands ep0/ep1/ep2 over in a single
// sendmsg so they always arrive as one set.
void send_fds(int sock, const int* fds, size_t n) {
    char byte = 'f';
    struct iovec io { &byte, 1 };
    char cmsgbuf[CMSG_SPACE(sizeof(int) * kMaxFds)];
    std::memset(cmsgbuf, 0, sizeof(cmsgbuf));

    struct msghdr msg {};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * n);
    std::memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * n);
    msg.msg_controllen = cmsg->cmsg_len;

    if (::sendmsg(sock, &msg, 0) < 0)
        diep("sendmsg");
}

size_t recv_fds(int sock, int* out, size_t max) {
    char byte = 0;
    struct iovec io { &byte, 1 };
    char cmsgbuf[CMSG_SPACE(sizeof(int) * kMaxFds)];
    std::memset(cmsgbuf, 0, sizeof(cmsgbuf));

    struct msghdr msg {};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    if (::recvmsg(sock, &msg, 0) <= 0)
        diep("recvmsg");

    size_t got = 0;
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
            continue;
        const int* payload = reinterpret_cast<const int*>(CMSG_DATA(c));
        const size_t n = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < n && got < max; i++)
            out[got++] = payload[i];
    }
    return got;
}

void log_peercred(int sock) {
    struct ucred cred {};
    socklen_t len = sizeof(cred);
    if (::getsockopt(sock, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) {
        std::printf("server: SO_PEERCRED — pid=%d uid=%d gid=%d\n",
                    static_cast<int>(cred.pid), static_cast<int>(cred.uid),
                    static_cast<int>(cred.gid));
    } else {
        std::printf("server: SO_PEERCRED unavailable: %s\n", std::strerror(errno));
    }
}

int create_seeded(const char* path, const char* seed) {
    // O_EXCL: root never follows a pre-planted /tmp symlink.
    int fd = ::open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        std::fprintf(stderr, "server: open(%s): %s\n", path, std::strerror(errno));
        std::exit(1);
    }
    if (::ftruncate(fd, 0) < 0)
        diep("ftruncate");
    if (::write(fd, seed, std::strlen(seed)) < 0)
        diep("write seed");
    struct stat st {};
    ::fstat(fd, &st);
    std::printf("server: created %s mode=%04o uid=%u gid=%u\n", path,
                static_cast<unsigned>(st.st_mode & 07777),
                static_cast<unsigned>(st.st_uid), static_cast<unsigned>(st.st_gid));
    return fd;
}

int serve(const char* prog, const char* sock_path) {
    if (::geteuid() != 0) {
        std::fprintf(stderr, "serve must run as root: sudo %s serve\n", prog);
        return 1;
    }

    // One visible file, opened twice — read-only and read-write — so the client
    // can tell the two access modes apart.
    const int seeded = create_seeded(kFilePath, "seed-A-from-root\n");
    const int fd_ro = ::open(kFilePath, O_RDONLY);
    const int fd_rw = ::open(kFilePath, O_RDWR);
    ::close(seeded);

    int fd_gone = create_seeded(kGonePath, "seed-C-path-deleted\n");
    if (::unlink(kGonePath) < 0)
        diep("unlink C");
    std::printf("server: unlinked %s — fd %d is all that is left of it\n",
                kGonePath, fd_gone);

    if (fd_ro < 0 || fd_rw < 0 || fd_gone < 0)
        diep("open");

    ::unlink(sock_path);  // stale socket from a previous run
    const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0) diep("socket");
    struct sockaddr_un addr {};
    fill_addr(addr, sock_path);
    if (::bind(listener, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
        diep("bind");
    // connect() needs write permission on the socket inode; who gets in is
    // decided by SO_PEERCRED, not by who can reach the path.
    if (::chmod(sock_path, 0666) < 0) diep("chmod");
    if (::listen(listener, 1) < 0) diep("listen");
    std::printf("server: listening on %s — now run the client as your desktop user\n",
                sock_path);

    const int conn = ::accept(listener, nullptr, nullptr);
    if (conn < 0) diep("accept");
    log_peercred(conn);

    const int fds[3] = { fd_ro, fd_rw, fd_gone };
    send_fds(conn, fds, 3);
    std::printf("server: sent fds {%d,%d,%d} in one sendmsg\n", fd_ro, fd_rw, fd_gone);

    // The client closes the connection when it is done; this read returns 0.
    char tail = 0;
    if (::read(conn, &tail, 1) < 0) diep("read conn");

    char buf[256] = {};
    const ssize_t n = ::pread(fd_rw, buf, sizeof(buf) - 1, 0);
    std::printf("server: file A now holds %zd bytes:\n---\n%s---\n",
                n > 0 ? n : 0, n > 0 ? buf : "(unreadable)");

    ::close(conn);
    ::close(listener);
    ::close(fd_ro);
    ::close(fd_rw);
    ::close(fd_gone);
    ::unlink(kFilePath);
    ::unlink(sock_path);
    return 0;
}

int client(const char* sock_path) {
    if (::geteuid() == 0) {
        std::fprintf(stderr, "client must NOT run as root — run it as your desktop user\n");
        return 1;
    }
    std::printf("client: uid=%d euid=%d gid=%d\n", ::getuid(), ::geteuid(), ::getgid());

    const int conn = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (conn < 0) diep("socket");
    struct sockaddr_un addr {};
    fill_addr(addr, sock_path);
    if (::connect(conn, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::fprintf(stderr, "client: connect(%s): %s\n", sock_path, std::strerror(errno));
        return 1;
    }

    int fds[3] = { -1, -1, -1 };
    const size_t got = recv_fds(conn, fds, 3);
    std::printf("client: recvmsg returned %zu fds {%d,%d,%d}\n", got,
                fds[0], fds[1], fds[2]);
    if (got != 3) return 1;

    char buf[128] = {};
    ssize_t n = ::pread(fds[0], buf, sizeof(buf) - 1, 0);
    std::printf("RESULT 1: read(O_RDONLY fd) %s: %s\n",
                n < 0 ? std::strerror(errno) : "OK", n > 0 ? buf : "");

    const char nope[] = "should-not-work\n";
    n = ::pwrite(fds[0], nope, sizeof(nope) - 1, 0);
    std::printf("RESULT 2: write(O_RDONLY fd) %s — expected EBADF, the access mode "
                "travelled with the file description\n",
                n < 0 ? std::strerror(errno) : "SUCCEEDED (?)");

    const char wrote[] = "written-by-uid-1000-through-passed-fd\n";
    n = ::pwrite(fds[1], wrote, sizeof(wrote) - 1, 0);
    std::printf("RESULT 3: write(O_RDWR fd) %s\n",
                n < 0 ? std::strerror(errno) : "OK");

    std::memset(buf, 0, sizeof(buf));
    n = ::pread(fds[2], buf, sizeof(buf) - 1, 0);
    std::printf("RESULT 4: read(fd whose path was unlinked) %s: %s\n",
                n < 0 ? std::strerror(errno) : "OK", n > 0 ? buf : "");

    const int mine = ::open(kFilePath, O_RDWR);
    std::printf("RESULT 5: open(\"%s\") myself %s — this decides whether a reopen "
                "after USB reconfig must always be done by the root side\n",
                kFilePath,
                mine < 0 ? std::strerror(errno)
                         : "SUCCEEDED (file is not root-only, probe proves nothing)");
    if (mine >= 0) ::close(mine);

    const int gone = ::open(kGonePath, O_RDWR);
    std::printf("RESULT 6: open(\"%s\") after unlink: %s — expected ENOENT\n",
                kGonePath, gone < 0 ? std::strerror(errno) : "still present (?)");
    if (gone >= 0) ::close(gone);

    ::close(fds[0]);
    ::close(fds[1]);
    ::close(fds[2]);
    ::close(conn);  // lets the server read file A back
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    // stderr is unbuffered: without this the RESULT lines and the error lines
    // interleave in the wrong order when the output is piped or copied.
    setvbuf(stdout, nullptr, _IOLBF, 0);

    if (argc < 2 || (std::strcmp(argv[1], "serve") != 0 &&
                     std::strcmp(argv[1], "client") != 0)) {
        std::fprintf(stderr, "usage: %s serve|client [socket-path]\n", argv[0]);
        return 1;
    }
    const char* sock_path = argc > 2 ? argv[2] : kSockPath;
    return std::strcmp(argv[1], "serve") == 0 ? serve(argv[0], sock_path)
                                             : client(sock_path);
}
