// THROWAWAY probe — the smallest possible version of "root opens, unprivileged
// process uses". No USB, no gadget, no FunctionFS semantics in the way: one
// root-owned 0600 file. Three questions, in the order that matters:
//
//   1. can a process that never had permission reach the data at all — via an
//      fd the privileged parent handed over SCM_RIGHTS? (the authorization-at-
//      open claim)
//   2. can that same process open() the path itself? (must be EACCES — this is
//      what decides whether recovery from "the fd died, reopen it" needs a
//      privileged helper forever)
//   3. did the parent keep its credentials while the child dropped its own?
//
// The child closes the fd it inherited from fork() before receiving, so any
// successful I/O can only be on the passed fd.
//
//   sudo ./build-spike/spike_fdpass_file [path]     # default: /tmp/ds5_fdpass_<pid>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

void send_fd(int sock, int fd)
{
    char byte = 'x';
    struct iovec io { &byte, 1 };
    char cmsgbuf[CMSG_SPACE(sizeof(fd))];
    std::memset(cmsgbuf, 0, sizeof(cmsgbuf));

    struct msghdr msg {};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(fd));
    std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
    msg.msg_controllen = cmsg->cmsg_len;

    if (::sendmsg(sock, &msg, 0) < 0) {
        std::fprintf(stderr, "parent: sendmsg: %s\n", std::strerror(errno));
    }
}

int recv_fd(int sock)
{
    char byte = 0;
    struct iovec io { &byte, 1 };
    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    std::memset(cmsgbuf, 0, sizeof(cmsgbuf));

    struct msghdr msg {};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    if (::recvmsg(sock, &msg, 0) <= 0) {
        std::fprintf(stderr, "child: recvmsg: %s\n", std::strerror(errno));
        return -1;
    }

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
        std::fprintf(stderr, "child: no SCM_RIGHTS in the message\n");
        return -1;
    }
    int fd = -1;
    std::memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
    return fd;
}

// setgroups before setgid/setuid: a stale supplementary group is still a way to
// open things, and once the uid is gone there is no going back.
bool drop_privileges()
{
    uid_t uid = 0;
    gid_t gid = 0;

    if (const char* override_name = std::getenv("FDPASS_USER")) {
        struct passwd* pw = ::getpwnam(override_name);
        if (!pw) {
            std::fprintf(stderr, "child: unknown user '%s'\n", override_name);
            return false;
        }
        uid = pw->pw_uid;
        gid = pw->pw_gid;
    } else {
        const char* su = std::getenv("SUDO_UID");
        const char* sg = std::getenv("SUDO_GID");
        if (!su || !sg) {
            std::fprintf(stderr, "child: SUDO_UID/SUDO_GID unset — run through sudo\n");
            return false;
        }
        uid = static_cast<uid_t>(std::strtoul(su, nullptr, 10));
        gid = static_cast<gid_t>(std::strtoul(sg, nullptr, 10));
    }

    if (::setgroups(0, nullptr) < 0) {
        std::fprintf(stderr, "child: setgroups: %s (continuing)\n", std::strerror(errno));
    }
    if (::setgid(gid) < 0 || ::setuid(uid) < 0) {
        std::fprintf(stderr, "child: setgid/setuid failed: %s\n", std::strerror(errno));
        return false;
    }
    std::printf("child: dropped — uid=%d euid=%d gid=%d egid=%d\n",
                ::getuid(), ::geteuid(), ::getgid(), ::getegid());
    return true;
}

int child_main(int sock, const char* path)
{
    if (!drop_privileges()) {
        return 2;
    }

    const int fd = recv_fd(sock);
    if (fd < 0) {
        return 3;
    }
    std::printf("child: received fd=%d flags=%#x\n", fd,
                static_cast<unsigned>(::fcntl(fd, F_GETFL)));

    // 1. read through the passed fd
    char buf[128] = {};
    const ssize_t n = ::pread(fd, buf, sizeof(buf) - 1, 0);
    if (n < 0) {
        std::printf("RESULT 1: pread on the received fd FAILED: %s\n", std::strerror(errno));
    } else {
        std::printf("RESULT 1: pread on the received fd got %zd bytes: \"%s\" — "
                    "uid %d read a file it has no permission on\n",
                    n, buf, ::getuid());
    }

    // 2. write through the passed fd
    const char msg[] = "hello-from-unprivileged-child\n";
    const ssize_t w = ::pwrite(fd, msg, sizeof(msg) - 1, n < 0 ? 0 : n);
    std::printf("RESULT 2: pwrite on the received fd %s\n",
                w < 0 ? std::strerror(errno) : "OK");

    // 3. the same path, opened by this process now
    const int mine = ::open(path, O_RDWR);
    if (mine < 0) {
        std::printf("RESULT 3: open(\"%s\") as uid %d FAILED: %s — so any future "
                    "reopen (USB reconfig, ep teardown) must be done by the privileged "
                    "side and passed over again\n", path, ::getuid(), std::strerror(errno));
    } else {
        std::printf("RESULT 3: open(\"%s\") as uid %d SUCCEEDED (fd=%d) — this file is "
                    "not root-only, the probe proves nothing\n", path, ::getuid(), mine);
        ::close(mine);
    }

    // 4. the boundary is not "all ops allowed once you hold the fd": ops that
    // check the *file's* ownership still see uid 1000 and fail.
    const int ch = ::fchmod(fd, 0666);
    std::printf("RESULT 4: fchmod through the received fd %s — expected: a use-time "
                "check against the file owner, so it must fail for a non-owner\n",
                ch < 0 ? std::strerror(errno) : "SUCCEEDED (we are the owner?!)");

    ::close(fd);
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    if (::geteuid() != 0) {
        std::fprintf(stderr, "must run as root: sudo %s [path]\n", argv[0]);
        return 1;
    }

    char fallback[64];
    std::snprintf(fallback, sizeof(fallback), "/tmp/ds5_fdpass_%d", ::getpid());
    const char* path = argc > 1 ? argv[1] : fallback;

    // Root creates a file only root can touch, and seeds it with a marker the
    // child could not possibly have written itself.
    const int fd = ::open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        std::fprintf(stderr, "parent: open(%s): %s\n", path, std::strerror(errno));
        return 1;
    }
    const char seed[] = "seeded-by-root-parent\n";
    if (::write(fd, seed, sizeof(seed) - 1) < 0) {
        std::fprintf(stderr, "parent: write: %s\n", std::strerror(errno));
    }
    struct stat st {};
    ::fstat(fd, &st);
    std::printf("parent: uid=%d created %s mode=%04o uid=%u gid=%u size=%lld\n",
                ::getuid(), path, st.st_mode & 07777, st.st_uid, st.st_gid,
                static_cast<long long>(st.st_size));

    // Lock the directory down too when we made the file under a shared /tmp:
    // mode 0600 is enough, but confirm what the child will see.
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        std::fprintf(stderr, "parent: socketpair: %s\n", std::strerror(errno));
        return 1;
    }

    const pid_t kid = ::fork();
    if (kid < 0) {
        std::fprintf(stderr, "parent: fork: %s\n", std::strerror(errno));
        return 1;
    }

    if (kid == 0) {
        ::close(sv[0]);
        ::close(fd);   // force the child onto the passed fd, not the inherited one
        const int rc = child_main(sv[1], path);
        ::close(sv[1]);
        _exit(rc);
    }

    ::close(sv[1]);
    send_fd(sv[0], fd);
    ::close(sv[0]);    // the fd lives in the child now; closing ours changes nothing there

    int status = 0;
    ::waitpid(kid, &status, 0);
    std::printf("parent: child exit=%d — parent still uid=%d euid=%d\n",
                WEXITSTATUS(status), ::getuid(), ::geteuid());

    // What did the child actually manage to write?
    char tail[128] = {};
    const ssize_t got = ::pread(fd, tail, sizeof(tail) - 1, 0);
    std::printf("parent: file now holds %zd bytes:\n---\n%s---\n",
                got > 0 ? got : 0, got > 0 ? tail : "(unreadable)");
    ::close(fd);

    if (argc <= 1) {
        ::unlink(path);   // our own temp file only; never unlink a path the user passed in
    }
    return 0;
}
