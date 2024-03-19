extern "C" {
#include "perf_dlfilter.h"
}

#include <cstring>

perf_dlfilter_fns perf_dlfilter_fns;

extern "C" {
  int filter_event(void *data, const struct perf_dlfilter_sample *sample,
                   void *ctx) {
    const struct perf_dlfilter_al *al;
    const struct perf_dlfilter_al *addr_al;

    /* keep non branch events */
    if (!sample->ip || !sample->addr_correlates_sym) return 0;

    al = perf_dlfilter_fns.resolve_ip(ctx);
    addr_al = perf_dlfilter_fns.resolve_addr(ctx);

    /* keep when either symbol is unknown */
    if (!al || !al->sym || !addr_al || !addr_al->sym) return 0;

    return !strcmp(al->sym, addr_al->sym);
  }
}
