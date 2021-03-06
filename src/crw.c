/**
 * crw.c controlled read/write functions
 * original source: simpletun
 *
 * (C) 2010 Davide Brini.
 * (C) 2017 - 2018 Erik Zscheile.
 *
 * License: GPL-3
 *
 * DISCLAIMER AND WARNING: this is all work in progress. The code is
 * ugly, the algorithms are naive, error checking and input validation
 * are very basic, and of course there can be bugs. If that's not enough,
 * the program has not been thoroughly tested, so it might even fail at
 * the few simple things it should be supposed to do right.
 * Needless to say, I take no responsibility whatsoever for what the
 * program might do. The program has been written mostly for learning
 * purposes, and can be used in the hope that is useful, but everything
 * is to be taken "as is" and without any kind of warranty, implicit or
 * explicit. See the file LICENSE for further details.
 **/

#include "crw.h"
#include <sys/types.h>
#include <sys/ioctl.h>
#include <net/if.h> // ifreq, IFNAMSIZ
#include <stdio.h>
#include <stdlib.h> // exit
#include <string.h>
#include <errno.h>
#include <unistd.h> // close, read
#include <fcntl.h>  // O_RDWR

int tun_alloc(char *dev, const int flags) {
  struct ifreq ifr;
  int err;

  const int fd = open("/dev/net/tun", O_RDWR);
  if(fd < 0) {
    perror("open(/dev/net/tun)");
    return fd;
  }

  memset(&ifr, 0, sizeof(ifr));
  ifr.ifr_flags = flags;

  if(*dev) strncpy(ifr.ifr_name, dev, IFNAMSIZ - 1);

  if( (err = ioctl(fd, TUNSETIFF, (void *)&ifr)) < 0 ) {
    perror("ioctl(TUNSETIFF)");
    close(fd);
    return err;
  }

  strcpy(dev, ifr.ifr_name);
  return fd;
}

int cread(const int fd, char *buf, const size_t n) {
  {
    const int cnt = read(fd, buf, n);
    if(cnt >= 0) return cnt;
  }
  printf("read() from fd %d failed: %s", fd, strerror(errno));
  exit(1);
}

// additional functions needed for work with UDP

// TODO: handle ICMP errmsg's with setsockopt IP_RECVERR and recvmsg MSG_ERRQUEUE
//       ex look @ https://stackoverflow.com/questions/11914568/read-icmp-payload-from-a-recvmsg-with-msg-errqueue-flag
int recv_n(const int fd, char * __restrict__ buf, const size_t n, struct sockaddr_storage * __restrict__ addr) {
  while(1) {
    socklen_t addrlen = sizeof(*addr);
    const int cnt = recvfrom(fd, buf, n, 0, (struct sockaddr *) addr, &addrlen);
    if(cnt > 0) return cnt;
  }
  return -1;
}
