/**
 * ping_cache.hpp
 * (C) 2018 Erik Zscheile.
 * License: GPL-2+
 **/
#pragma once
#include "iAFa.hpp"
#include "remote_peer.hpp"

class ping_cache_t final {
 public:
  struct match_t final {
    double diff;
    remote_peer_ptr_t router;
    uint8_t hops;
    bool match;
  };

  struct data_t final {
    inner_addr_t src, dst;
    uint16_t id, seq;

    data_t(): id(0), seq(0) { }
    data_t(const inner_addr_t &_src, const inner_addr_t &_dst,
           const uint16_t _id = 0, const uint16_t _seq = 0) noexcept
      : src(_src), dst(_dst), id(_id), seq(_seq) { }
  };

 private:
  double _seen;
  data_t _dat;
  remote_peer_ptr_t _router;

  static double get_ms_time() noexcept;

 public:
  ping_cache_t() noexcept: _seen(0) { }

  void init(const data_t &dat, const remote_peer_ptr_t &router) noexcept;
  auto match(const data_t &dat, const remote_peer_ptr_t &router, const uint8_t ttl)
       noexcept -> match_t;
};
