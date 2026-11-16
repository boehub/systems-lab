#include "live.h"

#include <stdio.h>
#include <string.h>

#include "pcap.h"

#ifdef __linux__
#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_packet.h>
#include <linux/sockios.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int live_open(const char *iface, int *linktype, char *err, size_t errsz) {
    unsigned idx = if_nametoindex(iface);
    if (!idx) {
        snprintf(err, errsz, "no such interface: %s", iface);
        return -1;
    }
    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) {
        snprintf(err, errsz, "cannot open a capture socket: %s%s", strerror(errno),
                 errno == EPERM || errno == EACCES ? " (live capture needs root)" : "");
        return -1;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", iface);
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) == 0 && ifr.ifr_hwaddr.sa_family != 1 /* Ethernet */ &&
        ifr.ifr_hwaddr.sa_family != 772 /* loopback */) {
        snprintf(err, errsz, "%s is not an Ethernet-style interface (only those are supported)", iface);
        close(fd);
        return -1;
    }
    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof sll);
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex = (int)idx;
    if (bind(fd, (struct sockaddr *)&sll, sizeof sll) != 0) {
        snprintf(err, errsz, "cannot bind to %s: %s", iface, strerror(errno));
        close(fd);
        return -1;
    }
    *linktype = LT_ETHERNET;
    return fd;
}

int live_read(int fd, uint8_t *buf, size_t cap, uint32_t *len, uint32_t *orig, int64_t *sec, uint32_t *nsec, int timeout_ms) {
    struct pollfd pfd = {fd, POLLIN, 0};
    int r = poll(&pfd, 1, timeout_ms);
    if (r == 0) return 0;
    if (r < 0) return errno == EINTR ? 0 : -1;
    struct sockaddr_ll from;
    socklen_t fl = sizeof from;
    ssize_t n = recvfrom(fd, buf, cap, MSG_TRUNC, (struct sockaddr *)&from, &fl);
    if (n < 0) return errno == EINTR ? 0 : -1;
    *orig = (uint32_t)n;
    *len = (size_t)n > cap ? (uint32_t)cap : (uint32_t)n;
    struct timespec ts;
    if (ioctl(fd, SIOCGSTAMPNS, &ts) != 0) clock_gettime(CLOCK_REALTIME, &ts);
    *sec = ts.tv_sec;
    *nsec = (uint32_t)ts.tv_nsec;
    // On the loopback interface every packet is seen twice (once leaving, once arriving);
    // keep only the outgoing copy so a conversation isn't doubled.
    if (from.sll_ifindex == (int)if_nametoindex("lo") && from.sll_pkttype != PACKET_OUTGOING) return 0;
    return 1;
}

void live_close(int fd) { close(fd); }

#else

int live_open(const char *iface, int *linktype, char *err, size_t errsz) {
    (void)iface;
    (void)linktype;
    snprintf(err, errsz, "live capture is only implemented on Linux");
    return -1;
}
int live_read(int fd, uint8_t *buf, size_t cap, uint32_t *len, uint32_t *orig, int64_t *sec, uint32_t *nsec, int timeout_ms) {
    (void)fd; (void)buf; (void)cap; (void)len; (void)orig; (void)sec; (void)nsec; (void)timeout_ms;
    return -1;
}
void live_close(int fd) { (void)fd; }

#endif
