#ifndef __REPLAY_HEADER__
#define __REPLAY_HEADER__

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <ios>
#include <string>
#include <map>
#include <vector>
#include <sstream>

#include "reader.hpp"
#include "perfetto.hpp"

struct Func {
  Symbol sym;
  std::vector<Func *> callee;
  Func *caller;
  size_t call_address;
  size_t tid; /* only meaningful when function is active */

  Time first_start = UINT64_MAX;
  /* most recent start and end time, only meaningful before merging functions */
  Time start = UINT64_MAX;
  Time end = 0;
  bool start_is_inferred = false;
  bool end_is_inferred = false;

  struct Statistics {
    Time sum_inferred = 0;
    Time sum = 0;
    size_t invoked = 0;
    size_t inferred = 0; /* call/ret time of this function is inferred */
    size_t n() { return invoked - inferred; }
    double average() {
      if (n() == 0) return 0;
      return static_cast<double>(sum) / (invoked - inferred);
    }

    void add_sample(Time t, bool inferred_sample) {
      invoked++;
      sum_inferred += t;
      if (!inferred_sample) {
        sum += t;
      } else inferred++;
    }

    void merge_stat(const Statistics &s) {
      sum_inferred += s.sum_inferred;
      sum += s.sum;
      invoked += s.invoked;
      inferred += s.inferred;
    }

    std::string stat_string() {
      std::ostringstream str;
      str << invoked;
      if (inferred) str << '(' << inferred << ')';
      if (n() > 1)
        str << ",avg:" << std::setprecision(0) << std::fixed << average();
      return str.str();
    }
  } stats;

  Func(Symbol s, Func *c, Time t, size_t tid):
    sym(s), caller(c), first_start(t), start(t), tid(tid) {}
  ~Func() {
    for (auto &f : callee) if (f->caller == this) delete f;
  }

  void destructive_merge(Func *);
  static Func *destructive_merge_funcs(std::vector<Func *> &);
  Func *call(const Symbol &, const Symbol &, Time);
  Func *ret(Time);

  void pretty_print(std::ostream &, std::string);
  void _flame_graph(std::ostream &, std::string, bool hide_zero);
  void flame_graph(std::ostream &);

  Time self_time(); /* calculate self latency during invokes */
  Func *find_callee(const Symbol &);
  Time last_time(); /* approximate return time of not-returned functions */

  typedef bool(Func::*FuncPred)(const Symbol &) const;
  static const size_t no_limit = UINT64_MAX;
  Func *find_caller(size_t, const Symbol &, FuncPred);
  bool name_match(const Symbol &s) const { return sym.name == s.name; }
  bool strict_name_match(const Symbol &s) const {
    if (sym.is_unknown() && s.is_unknown()) {
      // FIXME: magic number
      return std::abs(static_cast<int64_t>((sym.base() - s.base()))) < 0x10000;
    } else return sym.name == s.name;
  }
  bool base_match(const Symbol &s) const { return sym.base() == s.base(); }
  bool ret_addr_match(const Symbol &s) const {
    /* call on x86 is 5 bytes */
    return call_address && call_address <= s.address &&
           call_address + 10 > s.address;
  }
};

class History {
  Func *root;
  Func *current;
  size_t cpu;
  size_t tid;

  /* handle trace stop and start */
  bool in_syscall = false; /* used in user only trace */
  size_t pause_address = 0;
  Time pause_time;
  bool after_syscall = false; /* used in kernel trace */
  bool task_switch_flush_task = false;
  bool spinlock_mitigate = false; /* trace somehow broke in spin lock slow path */
  bool perf_event_switch_output = false;
  size_t enter_lazy_tlb = 0;
  std::vector<std::string> try_match_stack;
  Time time;

  void make_new_root(const Symbol &);
  bool call(const Symbol &, const Symbol &, Time);
  bool ret(const Symbol &, const Symbol &, Time);

public:
  size_t current_depth();
  void snapshot(std::ostream &);
  void print_status(std::ostream &os);
  History(const Symbol &, Time, size_t, size_t);
  History(const Action &a) : History(a.to, a.ts, a.cpu, a.tid) {}
  bool replay(const Action &);
  Func *terminate();
};

class Replay {
  std::map<size_t, History> threads;
  std::map<size_t, Time> last_seen;
  void stop_and_archive(size_t);

public:
  ~Replay() { for (auto r: archive) delete r; }
  std::vector<Func *> archive;
  bool replay(const Action &action);
  void cleanup() {
    while (!threads.empty()) stop_and_archive(threads.begin()->first);
  }
  Func *destructive_merge_all() {
    return Func::destructive_merge_funcs(archive);
  }
  void snapshot(std::ostream &, Time);
};

class ParallelReplay {
  struct AsyncReplay {
    Replay rp;
    std::thread thr;
    std::queue<Action> actions;
    std::mutex lock;
    std::condition_variable empty;
  };
  std::vector<AsyncReplay *> rps;

  std::atomic<bool> stop;
  size_t rotate_thread{0};

  static void replay_worker(AsyncReplay *, std::atomic<bool> *);
public:
  ParallelReplay(size_t);
  ~ParallelReplay();
  void deliver_action(Action &);
  void wait_all();
  Func *destructive_merge_all();
};

#endif
