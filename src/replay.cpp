#include <cassert>
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include <pthread.h>

#include "replay.hpp"
#include "perfetto.hpp"
#include "reader.hpp"

/* fake root function with impossible non-zero address */
static const Symbol global_root_function = {"/global_root/", 0x10, 0};
static const Symbol suspended_function = {"/suspended/", 0x20, 0};
static const std::string perf_event_switch_symbol = "perf_event_switch_output";
static const size_t try_match_max_depth = 10;

void Func::destructive_merge(Func *that) {
  if (!that) return;
  stats.merge_stat(that->stats);

  while (!that->callee.empty()) {
    auto f = that->callee.back();
    auto call_f = find_callee(f->sym);
    if (call_f) call_f->destructive_merge(f);
    else {
      callee.push_back(f);
      f->caller = this;
    }
    that->callee.pop_back();
  }
  delete that;
}

Func *Func::destructive_merge_funcs(std::vector<Func *> &fs) {
  if (fs.empty()) return nullptr;
  Func *root = fs[0];
  for (size_t i = 1; i < fs.size(); ++i)
    root->destructive_merge(fs[i]);
  fs.clear();
  return root;
}

Func *Func::call(const Symbol &from, const Symbol &s, Time ts) {
  call_address = from.address;
  auto f = find_callee(s);
  if (f) {
    f->start = ts;
    f->end = 0;
    f->start_is_inferred = false;
  } else {
    f = new Func(s, this, ts, tid);
    callee.push_back(f);
  }

  if (perfetto)
    perfetto->emit_function(tid, tid, f->sym.name, ts, Perfetto::EventType::BEGIN);
  return f;
}

Func *Func::ret(Time ts) {
  if (start > ts) {
    std::cerr << "Warning: function " << sym.name << " return time " << ts
              << " earlier than start " << start << std::endl << std::flush;
    stats.add_sample(0, true);
  }
  else stats.add_sample(ts - start, start_is_inferred || end_is_inferred);
  end = ts;
  start = UINT64_MAX;
  if (caller) caller->call_address = 0;

  if (perfetto) {
    if (start_is_inferred)
      perfetto->emit_function(tid, tid, sym.name, start, Perfetto::EventType::COMPLETE, ts);
    else
      perfetto->emit_function(tid, tid, sym.name, ts, Perfetto::EventType::END);
  }
  return caller;
};

void Func::pretty_print(std::ostream &os, std::string prefix) {
  os << prefix << sym.name << " : called " << stats.invoked
     << " lat " << stats.sum_inferred << std::endl;
  for (auto f: callee) f->pretty_print(os, prefix + "  ");
}

void Func::flame_graph(std::ostream &os) {
  /* skips /global_root/ */
  for (auto f: callee) f->_flame_graph(os, "", true);
}

void Func::_flame_graph(std::ostream &os, std::string prefix, bool hide_zero) {
  if (stats.sum_inferred == 0) return;
  std::string display_name = sym.name + ':' + stats.stat_string();
  os << prefix << display_name << ' ' << self_time() << std::endl;
  for (auto f: callee)
    f->_flame_graph(os, prefix + display_name + ';', hide_zero);
}

Time Func::self_time() {
  Time other = 0;
  for (auto i: callee) other += i->stats.sum_inferred;
  if (stats.sum_inferred < other) {
    std::cerr << "Total time less than other time for " << sym.name << " total "
              << stats.sum_inferred << " other " << other << std::endl
              << std::flush;
    return 0;
  }
  return stats.sum_inferred - other;
}

Func *Func::find_callee(const Symbol &s) {
  for (auto f: callee) if (f->sym.base() == s.base()) return f;
  for (auto f: callee) if (f->name_match(s)) return f;
  return nullptr;
}

Time Func::last_time() {
  Time t = start;
  for (auto &f: callee) t = std::max(t, f->end);
  return t;
}

Func *Func::find_caller(size_t limit, const Symbol &s, FuncPred pred) {
  auto f = this;
  size_t counter = 0;
  while (f && (counter++ < limit) & !(f->*pred)(s)) f = f->caller;
  return counter > limit ? nullptr : f;
}

void History::make_new_root(const Symbol &s) {
  /* call time of new root is unavailable, use initial call time of old root
   * HACK: minus 1 ns to distinguish two starts for perfetto
   * called symbol should have offset = 0 (we don't call mid of a function) */
  auto new_root =
      new Func({s.name, s.address - s.offset, 0}, nullptr,
               root->first_start - 1, tid);
  new_root->start_is_inferred = true;
  root->caller = new_root;
  new_root->callee.push_back(root);
  root = new_root;
}

bool History::call(const Symbol &from, const Symbol &to, Time ts) {
  /* look into stack for matched call site */
  /* first try to find exact match */
  auto f = current->find_caller(Func::no_limit, from, &Func::base_match);
  /* next try to find matched name */
  if (!f) f = current->find_caller(Func::no_limit, from, &Func::name_match);
  if (!f) return false;

  /* return to matched level, likely current_level */
  while (f != current) current = current->ret(ts);
  current = current->call(from, to, ts);
  return true;
}

bool History::ret(const Symbol &from, const Symbol &to, Time ts) {
  /* be less strict at bottom of stack */
  /* if no caller, infer caller from ret target */
  if (!current->caller) {
    current->ret(ts);
    make_new_root(to);
    current = root;
    return true;
  }

  /* look into stack for matched return target, and deprioritize current */
  auto f = current->caller->find_caller(Func::no_limit, to,
                                        &Func::ret_addr_match);
  if (!f && current->ret_addr_match(to)) f = current;
  if (!f) f = current->caller->find_caller(Func::no_limit, to,
                                           &Func::name_match);
  if (!f && current->name_match(to)) f = current;

  if (!f) return false;

  /* return to matched level, likely current_level->caller */
  while (f != current) current = current->ret(ts);
  return true;
}

History::History(const Symbol &s, Time ts, size_t c, size_t t):
  cpu(c), time(ts), tid(t) {
  root = current =
      new Func({s.name, s.address - s.offset, 0}, nullptr, ts, tid);
}

bool History::replay(const Action &action) {
  bool delete_hist = false;
  bool r = false;
  /* tracing was stopped but never started */
  if ((in_syscall || pause_address) && action.inst != Action::TR_START)
    return false;

  /* kernel trace mitigations from limited test on alikernel 5.10 */
  if (task_switch_flush_task) {
    /* special case for kprobe_flush_task and prepare_task_switch
       this is callback(?) hook for kernel tasks and is checked just before
       task switch returns to user thread. trace is briefly disabled(?) and we
       lost one or two stack level.
       we don't care about perf copying data (and other kernel tasks), so don't
       attempt to recreate stack during this function
     */
    if (action.inst != Action::RET) return true;
    if (action.to.name == "finish_task_switch") {
      /* stack: * > __schedule > finish_task_switch > kprobe_flush_task */
      task_switch_flush_task = false;
      return ret(current->sym, action.to, action.ts);
    } else if (action.to.name == "prepare_task_switch") {
      /* stack: * > __schedule > prepare_task_switch */
      task_switch_flush_task = false;
    }
    return true;
  } else if (enter_lazy_tlb) {
    /* special case for enter_lazy_tlb, where kernel will soon reschedule (?)
       tid may not be accurate (?), trace breaks repeatedly, so wait for this
       pattern before continuing
       tr strt [unknown] -> schedule
       return  schedule -> <some symbol in call stack> */
    if (enter_lazy_tlb == 1) {
      if (action.inst != Action::TR_START) return true; /* ignore */
      if (!action.from.is_unknown()) {
        /* data loss */
        enter_lazy_tlb = 0;
        return false;
      }
      if (action.to.name != "schedule") return true; /* ignore */
      enter_lazy_tlb = 2;
      return true;
    } else {
      enter_lazy_tlb = 0;
      switch (action.inst) {
      case Action::CALL:
        enter_lazy_tlb = 1;
        return true;
      case Action::RET:
        if (action.from.name != "schedule") return false;
        return ret(action.from, action.to, action.ts);
      default: return false;
      }
    }
  } else if (perf_event_switch_output) {
    perf_event_switch_output = false;
    /* handle this specific case:
       tr strt  [unknown] -> perf_event_switch_output
       return   perf_event_switch_output -> <some symbol in call stack> */
    if (action.inst != Action::RET ||
        action.from.name != perf_event_switch_symbol)
      return false;
    /* from won't match current, but History::ret handles this discrepancy */
    return ret(action.from, action.to, action.ts);
  }

  /* trace around syscall looks like
     1. syscall __libc_recv+0x79 => __entry_text_start+0x0
     2. call entry_SYSCALL_64_after_hwframe+0x3f => do_syscall_64+0x0
     there is a known symbol mismatch, insert a call to connect stack
   */
  if (after_syscall) {
    if (action.inst != Action::CALL) return false;
    if (current->sym != action.from)
      if (!call(current->sym, action.from, action.ts)) return false;
    after_syscall = false;
  }

  switch (action.inst) {
  case Action::TR_END_SYSCALL:
    in_syscall = true;
    return call(action.from, action.to, action.ts);
  case Action::SYSCALL:
    after_syscall = true;
    /* fallthrough */
  case Action::JCC: case Action::JMP:
  /* if jump site and target do not match, treat jump as a call,
     this happens a lot in library functions.
     NOTE: redundant jump is filtered in reader */
  case Action::INT: case Action::CALL:
    return call(action.from, action.to, action.ts);
  case Action::SYSRET:
    in_syscall = false;
  /* fallthrough */
  case Action::RET: case Action::IRET:
    return ret(action.from, action.to, action.ts);
  case Action::TR_END:
    pause_address = action.from.address;
    pause_time = action.ts;
    return call(action.from, suspended_function, action.ts);
  case Action::TR_START:
    if (in_syscall) {
      /* resuming from syscall */
      in_syscall = false;
      return ret(action.from, action.to, action.ts);
    } else if (pause_address && pause_address == action.to.address) {
      /* resuming from trace end, do nothing */
      pause_address = 0;
      return ret(suspended_function, action.to, action.ts);
    } else if (current->sym.name == "kprobe_flush_task" ||
               current->sym.name == "prepare_task_switch") {
      task_switch_flush_task = true;
      return true;
    } else if (current->sym.name == "enter_lazy_tlb") {
      enter_lazy_tlb = 1;
      return true;
    } else if (action.from.is_unknown() &&
               action.to.name == perf_event_switch_symbol) {
      perf_event_switch_output = true;
      return true;
    } else if (action.from.base() == 0 && action.to.is_unknown()) {
      /* special case for
         call     clock_gettime@GLIBC_2.2.5 => __vdso_clock_gettime
         tr strt  0 [unknown] => 7fff56f8ca49 [unknown]

         fake a call from __vdso_clock_gettime to unknown
       */
      return call(current->sym, action.to, action.ts);
    }
    return false;
  case Action::END: return false;
  }
  return false;
}

Func *History::terminate() {
  /* end all currently open function calls and accumulate latencies
     estimate low bound of return time */
  Time ts = pause_address ? pause_time : current->last_time();
  while (current != root) {
    current->end_is_inferred = true;
    current = current->ret(ts);
  }
  /* install current root to a dummy global root so we can merge everything */
  ret(root->sym, global_root_function, ts);
  root->ret(ts);
  return root;
}

size_t History::current_depth() {
  auto c = current;
  size_t count = 0;
  while (c) {
    count++;
    c = c->caller;
  }
  return count;
}

void History::print_status(std::ostream &os) {
  auto f = current;
  os << "STACK: ";
  while (f) {
    os << f->sym.name << " ";
    f = f->caller;
  }
  os << std::endl;
}

void History::snapshot(std::ostream &os) {
  auto c = current;
  while (c) {
    os << c->sym.name << std::endl;
    c = c->caller;
  }
}

void Replay::stop_and_archive(size_t tid) {
  auto root = threads.at(tid).terminate();
  archive.push_back(root);
  threads.erase(tid);
}

bool Replay::replay(const Action &action) {
  auto hist = threads.find(action.tid);
  if (hist == threads.end()) {
    if (action.to.is_unknown()) return true;
    /* found new thread */
    threads.emplace(std::make_pair(action.tid, History(action)));
  } else if (!hist->second.replay(action)) {
    // if (hist->second.current_depth() > 2) {
    //   std::cerr << "TRACE BROKEN for tid " << action.tid << std::endl;
    //   hist->second.print_status(std::cerr);
    //   Action::Inst insn;
    //   std::cerr << action.from.name << " " << action.inst << " -> "
    //             << action.to.name << " ts " << action.ts << std::endl
    //             << std::endl;
    // }
    // archive current history and start a new one for current thread
    stop_and_archive(action.tid);
    threads.emplace(std::make_pair(action.tid, History(action)));
  }
  last_seen[action.tid] = action.ts;
  return true;
}

ParallelReplay::ParallelReplay(size_t worker): stop(false) {
  for (size_t i = 0; i < worker; ++i) {
    rps.push_back(new AsyncReplay);
    rps[i]->thr = std::thread(replay_worker, rps[i], &stop);
    pthread_setname_np(rps[i]->thr.native_handle(), "Replay");
  }
}

ParallelReplay::~ParallelReplay() {
  stop.store(true);
  for (auto rp: rps) {
    rp->empty.notify_one();
    rp->thr.join();
    delete rp;
  }
}

void ParallelReplay::replay_worker(AsyncReplay *rp, std::atomic<bool> *stop) {
  while (!stop->load()) {
    std::unique_lock<std::mutex> ul(rp->lock);
    while (rp->actions.empty() && !stop->load()) rp->empty.wait(ul);
    if (stop->load()) return;
    auto action = rp->actions.front();
    rp->actions.pop();
    ul.unlock();
    (void) rp->rp.replay(action);
  }
}

void ParallelReplay::deliver_action(Action &action) {
  size_t idx = action.tid % rps.size();
  rps[idx]->lock.lock();
  rps[idx]->actions.push(action);
  rps[idx]->lock.unlock();
  rps[idx]->empty.notify_one();
}

void ParallelReplay::wait_all() {
  for (auto rp: rps) {
    rp->lock.lock();
    while (!rp->actions.empty()) {
      rp->lock.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      rp->lock.lock();
    }
    rp->lock.unlock();
  }
}

Func *ParallelReplay::destructive_merge_all() {
  std::vector<std::thread> merges(rps.size());
  std::vector<Func *> roots(rps.size());
  auto do_merge = [](Replay *rp, Func **root) {
    rp->cleanup();
    *root = rp->destructive_merge_all();
  };
  for (size_t i = 0; i < rps.size(); ++i)
    merges[i] = std::move(std::thread(do_merge, &rps[i]->rp, &roots[i]));
  for (auto &m: merges) m.join();
  return Func::destructive_merge_funcs(roots);
}

void Replay::snapshot(std::ostream &os, Time ts) {
  os << "timestamp " << pretty_time(ts) << std::endl;
  for (auto &[tid, hist] : threads) {
    os << tid << " last seen " << pretty_time(last_seen[tid])
       << " Δ " << pretty_time(ts - last_seen[tid]) << std::endl;
    hist.snapshot(os);
    os << std::endl;
  }
}
