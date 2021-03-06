/**
 * zprn.cxx
 * (C) 2018 Erik Zscheile.
 * License: GPL-2+
 **/

#include "zprn.hpp"

bool zprn_v2hdr::valid() const noexcept
  { return (!zprn_mgc && zprn_ver == 2); }

size_t zprn_v2::get_needed_size() const noexcept
  { return 2 + route.get_tflen(); }
