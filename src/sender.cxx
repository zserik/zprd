/**
 * zprd / sender.cxx
 * (C) 2018 Erik Zscheile.
 * License: GPL-2+
 **/

#define __USE_MISC 1
#include <sys/types.h>
#include "sender.hpp"
#include "crest.h"
#include <zs/ll/memut.hpp>
#include <config.h>
#include <stdio.h>       // perror
#include <unistd.h>      // write
#include <sys/prctl.h>   // prctl
#include <netinet/ip.h>  // struct ip, IP_*
#include <algorithm>     // remove_if

using namespace std;

void sender_t::enqueue(send_data &&dat) {
  // sanitize dat.dests
  if(dat.dests.empty())
    return;
  if(dat.dests.front()->is_local())
    dat.dests.clear();
  dat.dests.shrink_to_fit();

  // move into queue
  {
    lock_guard<mutex> lock(_mtx);
    _tasks.emplace_back(move(dat));
  }
  _cond.notify_one();
}

void sender_t::enqueue(zprn2_sdat &&dat) {
  // sanitize dat.dests
  {
    const auto ie = dat.dests.end();
    dat.dests.erase(remove_if(dat.dests.begin(), ie,
      [](const auto &x) noexcept { return zs_unlikely(!x) || x->is_local(); }),
      ie);
  }
  if(dat.dests.empty())
    return;
  dat.dests.shrink_to_fit();

  // move into queue
  {
    lock_guard<mutex> lock(_mtx);
    _zprn_msgs.emplace_back(move(dat));
  }
  _cond.notify_one();
}

void sender_t::start() {
  {
    lock_guard<mutex> lock(_mtx);
    _stop = false;
  }
  thread(&sender_t::worker_fn, this).detach();
}

void sender_t::stop() noexcept {
  {
    lock_guard<mutex> lock(_mtx);
    _stop = true;
  }
  _cond.notify_all();
}

#include <unordered_set>
#include <unordered_map>

/** file descriptors
 *
 * local_fd   = the tun device
 * server_fds = the server udp sockets
 **/
extern int local_fd;
extern unordered_map<sa_family_t, int> server_fds;

void sender_t::worker_fn() noexcept {
  // create a backup
  const auto my_server_fds = server_fds;

  unordered_set<remote_peer_ptr_t> zprn_confirmed;
  bool got_error = false, df = false;
  uint32_t tos = 0;

  const auto sendto_peer = [&](const remote_peer_ptr_t &i, const vector<char> &buf) noexcept {
    const auto confirmed_it = zprn_confirmed.find(i);
    const bool is_confirmed = (confirmed_it != zprn_confirmed.end());
    if(is_confirmed) zprn_confirmed.erase(confirmed_it);
    return i->locked_crun([&](const remote_peer_t &o) noexcept {
      if(zs_unlikely(o.is_local())) {
        fprintf(stderr, "SENDER INTERNAL ERROR: destination peer is local, use count = %ld, size = %zu\n", i.use_count(), buf.size());
        return;
      }
      const auto fdit = my_server_fds.find(o.saddr.ss_family);
      if(zs_unlikely(fdit == my_server_fds.end())) {
        fprintf(stderr, "SENDER INTERNAL ERROR: destination peer with unknown address family %u, size = %zu\n",
          static_cast<unsigned>(o.saddr.ss_family), buf.size());
        return;
      }
      if(zs_unlikely(sendto(
          fdit->second, buf.data(), buf.size(), is_confirmed ? MSG_CONFIRM : 0,
          reinterpret_cast<const struct sockaddr *>(&o.saddr), sizeof(o.saddr)) < 0))
      {
        perror("sendto()");
        got_error = true;
      }
    });
  };

  prctl(PR_SET_NAME, "sender", 0, 0, 0);

  const int fd_inet = my_server_fds.at(AF_INET);
#ifdef USE_IPV6
  const auto fdx_inet6 = ([&my_server_fds]() noexcept -> pair<bool, int> {
    const auto it = my_server_fds.find(AF_INET6);
    const bool x = (it != my_server_fds.end());
    return { x, x ? it->second : 0 };
  })();
#endif

  const auto set_df = [&](const bool cdf) noexcept {
    const int tmp_df = cdf
# if defined(IP_DONTFRAG)
      ;
    if(setsockopt(fd_inet, IPPROTO_IP, IP_DONTFRAG, &tmp_df, sizeof(tmp_df)) < 0)
      perror("SENDER WARNING: setsockopt(IP_DONTFRAG) failed");
# elif defined(IP_MTU_DISCOVER)
      ? IP_PMTUDISC_WANT : IP_PMTUDISC_DONT;
    if(setsockopt(fd_inet, IPPROTO_IP, IP_MTU_DISCOVER, &tmp_df, sizeof(tmp_df)) < 0)
      perror("SENDER WARNING: setsockopt(IP_MTU_DISCOVER) failed");
# else
#  warning "set_ip_df: no method available to manage the dont-frag bit"
      ;
    if(0) {}
# endif
    else df = cdf;
  };

  const auto set_tos = [&](const uint32_t ctos) noexcept {
    // ignore failure of set_tos
    const uint8_t ip4_tos = tos = ctos;
    if(setsockopt(fd_inet, IPPROTO_IP, IP_TOS, &ip4_tos, 1) < 0) {
      perror("SENDER WARNING: setsockopt(IP_TOS) failed");
      got_error = true;
    }
#ifdef USE_IPV6
    if(fdx_inet6.first && setsockopt(fdx_inet6.second, IPPROTO_IPV6, IPV6_TCLASS, &ctos, sizeof(ctos)) < 0) {
      perror("SENDER WARNING: setsockopt(IPV6_TCLASS) failed");
      got_error = true;
    }
#endif
  };

  const auto zprn_rttr = [](zprn2_sdat &i) noexcept {
    auto &x = i.zprn.route.type;
    x = htons(x);
  };

  set_df(false);
  set_tos(0);

  vector<send_data> tasks;
  vector<zprn2_sdat> zprn_msgs;
  unordered_map<remote_peer_ptr_t, vector<vector<char>>> zprn_buf;
  const auto zprn_hdrv = ([]() -> vector<char> {
    zprn_v2hdr x_zprn;
    zeroify(x_zprn);
    x_zprn.zprn_ver = 2;
    const auto h_zprn = reinterpret_cast<const char *>(&x_zprn);
    return vector<char>(h_zprn, h_zprn + sizeof(x_zprn));
  })();

  while(true) {
    {
      unique_lock<mutex> lock(_mtx);
      _cond.wait(lock, [this] { return _stop || !(_tasks.empty() && _zprn_msgs.empty()); });
      if(zs_unlikely(_tasks.empty() && _zprn_msgs.empty())) return;
      tasks = move(_tasks);
      zprn_msgs = move(_zprn_msgs);
    }

    got_error = false;

    // send normal data
    for(auto &dat: tasks) {
      // NOTE: it is impossible that local_ip and others are destinations together
      if(dat.dests.empty()) {
        auto buf = dat.buffer.data();
        const auto buflen = dat.buffer.size();

        { // update checksum if ipv4
          const auto h_ip = reinterpret_cast<struct ip*>(buf);
          if(buflen >= sizeof(struct ip) && h_ip->ip_v == 4)
            h_ip->ip_sum = IN_CKSUM(h_ip);
        }
        if(zs_unlikely(write(local_fd, buf, buflen) < 0)) {
          got_error = true;
          perror("write()");
        }
        continue;
      }

      // setup outer TOS
      if(tos != dat.tos) set_tos(dat.tos);

      { // setup outer Dont-Frag bit
        const bool cdf = dat.frag & htons(IP_DF);
        if(df != cdf) set_df(cdf);
      }

      for(const auto &i : dat.dests)
        sendto_peer(i, dat.buffer);
    }

    if(zprn_msgs.empty()) goto flush_stdstreams;
    tasks.clear();

    // setup outer Dont-Frag bit + TOS
    if(df)  set_df(false);
    if(tos) set_tos(0);

    // build ZPRN v2 messages for each destination
    if(zprn_msgs.size() == 1) {
      // skip concat part
      vector<char> xbuf = zprn_hdrv;
      auto &i = zprn_msgs.front();
      if(i.confirmed) zprn_confirmed.insert(i.confirmed);
      {
        const size_t zmsiz = i.zprn.get_needed_size();
        const char *const zmbeg = reinterpret_cast<const char *>(&i.zprn), *const zmend = zmbeg + zmsiz;
        // don't set this earlier, as we need thy host-byte-order.type in get_needed_size
        zprn_rttr(i);
        xbuf.reserve(xbuf.size() + zmsiz);
        xbuf.insert(xbuf.end(), zmbeg, zmend);
      }
      for(const auto &dest : i.dests)
        sendto_peer(dest, xbuf);

      zprn_msgs.clear();
      goto flush_stdstreams;
    }

    // NOTE: split zprn packet in multiple parts if it exceeds a certain size (e.g. 1232 bytes = 35 packets in worst case),
    //  but it is irrealistic, that this happens.
    //  This is important because IPv6 doesn't perform fragmentation.
    for(auto &i : zprn_msgs) {
      const size_t zmsiz = i.zprn.get_needed_size();
      zprn_buf.reserve(i.dests.size());
      zprn_rttr(i);
      const char *const zmbeg = reinterpret_cast<const char *>(&i.zprn), *const zmend = zmbeg + zmsiz;
      if(i.confirmed) zprn_confirmed.insert(i.confirmed);
      for(const auto &dest : i.dests) {
        auto &buffer = zprn_buf[dest];
        if(buffer.empty() || zs_unlikely((buffer.back().size() + zmsiz) > 1232)) {
          // create new buffer slot
          buffer.emplace_back(zprn_hdrv);
        }
        auto &bufitem = buffer.back();
        bufitem.reserve(bufitem.size() + zmsiz);
        bufitem.insert(bufitem.end(), zmbeg, zmend);
      }
    }

    zprn_msgs.clear();

    // send ZPRN v2 messages
    for(const auto &bufpd : zprn_buf)
      for(const auto &pkt : bufpd.second)
        sendto_peer(bufpd.first, pkt);

    zprn_buf.clear();

   flush_stdstreams:
    if(zs_unlikely(got_error)) {
      fflush(stdout);
      fflush(stderr);
    }
  }
}
