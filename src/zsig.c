#include "zsig.h"

// (C) 2017 Erik Zscheile
// (C) 2006 Rheinwerk Verlag GmbH

void my_signal(const int sig_nr, const sighandler_t sig_handler) {
  struct sigaction newsig;
  newsig.sa_handler = sig_handler;
  newsig.sa_flags   = SA_RESTART;
  sigemptyset(&newsig.sa_mask);

  struct sigaction oldsig;
  sigaction(sig_nr, &newsig, &oldsig);
}