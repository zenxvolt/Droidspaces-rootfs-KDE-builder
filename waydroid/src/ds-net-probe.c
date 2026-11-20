/*
 * ds-net-probe: open one TCP connection the way an Android app would, to find
 * out what stops app traffic when root traffic works.
 *
 *   ds-net-probe ADDRESS PORT [UID] [MARK]
 *
 * UID   run the connection as this user (e.g. 10100, an app), with the inet
 *       group Android gives apps that may use the network. Default: stay root.
 * MARK  socket mark, as netd sets it for apps (e.g. 0x64 for network 100).
 *       Default: none.
 *
 * Prints one line: "connected", "refused", "timeout" or the error. Comparing
 * root/app and with/without the mark tells routing problems (the mark) from
 * filtering by user (the UID) apart. Exit status 0 only when connected.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s ADDRESS PORT [UID] [MARK]\n", argv[0]);
        return 2;
    }
    struct sockaddr_storage ss;
    socklen_t len;
    memset(&ss, 0, sizeof(ss));
    int port = atoi(argv[2]);
    struct sockaddr_in *v4 = (struct sockaddr_in *)&ss;
    struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&ss;
    if (inet_pton(AF_INET, argv[1], &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port = htons((uint16_t)port);
        len = sizeof(*v4);
    } else if (inet_pton(AF_INET6, argv[1], &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons((uint16_t)port);
        len = sizeof(*v6);
    } else {
        fprintf(stderr, "not an IP address: %s\n", argv[1]);
        return 2;
    }

    int fd = socket(ss.ss_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        printf("socket: %s\n", strerror(errno));
        return 1;
    }
    if (argc > 4) {
        unsigned mark = (unsigned)strtoul(argv[4], NULL, 0);
        if (setsockopt(fd, SOL_SOCKET, SO_MARK, &mark, sizeof(mark)) < 0) {
            printf("SO_MARK: %s\n", strerror(errno));
            return 1;
        }
    }
    if (argc > 3) {
        uid_t uid = (uid_t)atoi(argv[3]);
        gid_t groups[] = { 3003 /* AID_INET */ };
        if (setgroups(1, groups) < 0 || setresgid(uid, uid, uid) < 0 ||
            setresuid(uid, uid, uid) < 0) {
            printf("cannot become uid %u: %s\n", (unsigned)uid, strerror(errno));
            return 1;
        }
    }

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    if (connect(fd, (struct sockaddr *)&ss, len) == 0) {
        printf("connected\n");
        return 0;
    }
    if (errno != EINPROGRESS) {
        printf("%s\n", errno == ECONNREFUSED ? "refused" : strerror(errno));
        return 1;
    }
    struct pollfd p = { .fd = fd, .events = POLLOUT };
    if (poll(&p, 1, 8000) <= 0) {
        printf("timeout\n");
        return 1;
    }
    int err = 0;
    socklen_t el = sizeof(err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
    if (err == 0) {
        printf("connected\n");
        return 0;
    }
    printf("%s\n", err == ECONNREFUSED ? "refused" : strerror(err));
    return 1;
}
