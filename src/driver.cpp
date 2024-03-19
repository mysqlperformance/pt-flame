#include <chrono>
#include <cstdlib>
#include <getopt.h>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "reader.hpp"
#include "replay.hpp"
#include "perfetto.hpp"

int main(int argc, char *argv[]) {
  /* flamegraph options */
  size_t limit = 0;
  size_t parallel = 0;
  size_t read_step = 10000;
  int cpu = -1;
  std::map<int, std::vector<std::string>> cpu_map = {{-1, {}}};

  /* print stack options */
  bool stack_print = false;
  std::string stack_prefix = "";
  size_t stack_warmup = 2000000;
  size_t stack_interval = 1000000;
  size_t stack_count = 1;
  size_t stack_printed = 0;
  Time stack_last_ts = 0;
  std::string stack_at_end = "";
  bool stack_only = false;

  std::string perfetto_file = "";

  int opt;
  while ((opt = getopt(argc, argv, "j:l:s:t:c:S:W:C:I:OP:E:")) != -1) {
    switch (opt) {
    case 'l': limit = std::stol(optarg); break;
    case 'j':
      parallel = std::stol(optarg);
      break;
    case 's': read_step = std::stol(optarg); break;
    case 'c': cpu = std::stol(optarg); break;
    case 't':
      if (cpu == -1) {
        std::cerr << "No cpu specified, use -c before -t\n";
        exit(EXIT_FAILURE);
      }
      {
        std::string tr_str = optarg;
        size_t tr_pos = 0;
        size_t tr_pos_end = 0;
        while (tr_pos < tr_str.size()) {
          tr_pos_end = tr_str.find(',', tr_pos);
          if (tr_pos_end == std::string::npos)
            tr_pos_end = tr_str.size();
          cpu_map[cpu].push_back(tr_str.substr(tr_pos, tr_pos_end - tr_pos));
          tr_pos = tr_pos_end + 1;
        }
      }
      break;
    case 'S': stack_print = true; stack_prefix = optarg; break;
    case 'W': stack_warmup = std::stol(optarg); break;
    case 'I': stack_interval = std::stol(optarg); break;
    case 'C': stack_count = std::stol(optarg); break;
    case 'O': stack_only = true; break;
    case 'E': stack_at_end = std::stol(optarg); break;
    case 'P': perfetto_file = optarg; break;
    default:
      std::cerr <<
      "Usage: pt_demo [-l limit] [-j parallel] [-s read_step] "
      "[-c cpu] [-t trace[,trace[...]]] [trace [trace [...]]]\n"
      "\n  FlameGraph Options: \n"
      "  -c <num> specifies CPU number for following traces, required before -t\n"
      "  -t <trace[,trace[...]]> trace files for a cpu, sequentially\n"
      "     this is designed to process multiple trace files from the same CPU\n"
      "     traces specified at end of command without -t is CPU-less and have no\n"
      "     ordering imposed. This is designed for small number of large traces.\n"
      "     do NOT mix -t trace and CPU-less trace\n"
      "  -l <num> limit number of instructions to replay, defaults no limit\n"
      "  -j <num> parallel worker to parse traces, this number is NOT a hard limit,\n"
      "     default 0, which turns off parallel. do NOT turn on parallel in production\n"
      "     if num > 0: \n"
      "       for EACH cpu (-c), spawn AT LEAST one worker to parse all traces\n"
      "       if only CPU-less trace is provided, spawn at least one worker to\n"
      "       parse EACH trace\n"
      "  -s <num> split trace files every num lines to replay, default 10000\n"
      "\n  Print Stack Options: \n"
      "  -S <prefix> print stacks to files named prefix_<seq#>, OVERWRITE\n"
      "     existing files. do NOT print if not set\n"
      "  -W <t> warmup, start printing after t ns, default 2000000 ns\n"
      "  -I <t> interval, print every t ns after warmup, default 1000000 ns\n"
      "  -C <num> print num number of stack, default 1\n"
      "  -E <name> print one stack to file named name at the end of replay\n"
      "  -O output stack only\n"
      "\n  Perfetto Options: \n"
      "  -P <name> output ftf (fuschia trace format) for use with Perfetto\n"
      "     don't output if not set\n";
      exit(EXIT_FAILURE);
    }
  }

  size_t streams = 0;
  if (cpu_map.size() > 1) {
    /* if -t trace is provided, ignore CPU-less trace */
    if (optind < argc)
      std::cerr << "Extra trace file at the end of command, ignore" << std::endl;
    streams = cpu_map.size() - 1;
  } else {
    /* otherwise, use CPU-less trace */
    while (optind < argc) cpu_map[-1].push_back(argv[optind++]);
    streams = cpu_map[-1].size();
  }

  size_t real_parallel = std::max(1UL, parallel / streams);
  if (parallel && real_parallel * streams > parallel)
    std::cerr << "Will spawn " << real_parallel * streams << " workers, more then"
              << " specifed number " << parallel;

  std::vector<GetAction *> trs;

  if (parallel) {
    if (cpu_map.size() == 1) {
      /* CPU-less traces */
      if (cpu_map[-1].empty()) trs.push_back(new StreamReader(&std::cin, read_step));
      else for (auto &f : cpu_map[-1])
        trs.push_back(new ParallelReader(f, real_parallel, read_step * 200));
    } else {
      /* ordered -t traces */
      for (auto &[num, fs]: cpu_map) {
        if (num == -1) continue;
        trs.push_back(new StreamReader(fs, real_parallel, read_step));
      }
    }
  } else {
    if (cpu_map.size() == 1) { /* CPU-less traces */
      if (cpu_map[-1].empty()) trs.push_back(new BasicReader(&std::cin));
      else for (auto &f : cpu_map[-1]) trs.push_back(new FileReader(f));
    } else { /* ordered -t traces */
      for (auto &[num, fs]: cpu_map) {
        if (num == -1) continue;
        trs.push_back(new FileReader(fs));
      }
    }
  }

  std::atomic<bool> stop_thread;
  std::atomic<bool> status_print;
  auto status_thread = [&]() {
    while (!stop_thread.load()) {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      status_print.store(true);
    }
  };
  std::thread status(status_thread);

  MergeWrapper mw(trs);

  size_t counter = 0;
  Action action;
  Replay rp;

  if (perfetto_file != "") {
    perfetto = new Perfetto(perfetto_file);
    perfetto->emit_magic();
  }

  Time last_ts;
  do {
    action = mw.next_action_by_block();
    if (action.inst == Action::END) break;
    last_ts = action.ts;
    rp.replay(action);

    /* pt_pstack */
    if (stack_print) {
      if (stack_printed < stack_count) {
        if (stack_last_ts == 0) stack_last_ts = action.ts;
        else if ((!stack_printed && action.ts - stack_last_ts > stack_warmup) ||
                 (stack_printed && action.ts - stack_last_ts > stack_interval)) {
          auto name = stack_prefix + std::to_string(stack_printed++);
          std::ofstream of(name);
          rp.snapshot(of, action.ts);
          std::cerr << "stack: " << name << std::endl;
          stack_last_ts = action.ts;
        }
      } else if (stack_only) break;
    }

    /* prints status info every 1 sec */
    if (status_print.load()) {
      std::cerr << "counter:" << counter << " ts " << pretty_time(action.ts) << std::endl;
      status_print.store(false);
    }
  } while (++counter < limit || limit == 0);

  std::cerr << "counter:" << counter << " ts " << pretty_time(action.ts) << std::endl;
  stop_thread.store(true);

  if (stack_at_end != "") {
    std::ofstream of(stack_at_end);
    rp.snapshot(of, last_ts);
  }

  rp.cleanup();

  if (!(stack_print && stack_only)) {
    auto root = rp.destructive_merge_all();
    root->flame_graph(std::cout);
  }

  for (auto tr: trs) delete tr;
  if (perfetto) delete perfetto;
  status.join();
  std::cerr << "done" << std::endl;
  return 0;
}
