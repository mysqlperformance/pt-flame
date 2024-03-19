#ifndef __READER_HEADER__
#define __READER_HEADER__

#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <istream>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
#include <queue>
#include <string>
#include <atomic>

typedef uint64_t Time;

std::string pretty_time(Time t);

struct Symbol {
  std::string name;
  uint64_t address;
  uint64_t offset;
  uint64_t base() const { return address ? address - offset : 0; }
  bool operator==(const Symbol &that) const { return address == that.address; }
  bool operator!=(const Symbol &that) const { return !(*this == that); }
  bool is_kernel() const { return static_cast<int64_t>(base()) < 0; }
  bool is_user() const { return static_cast<int64_t>(base()) > 0; }
  bool is_unknown() const { return name == "[unknown]"; }
};

struct Action {
  enum Inst {
    CALL, RET, JMP, JCC, TR_START, TR_END, TR_END_SYSCALL,
    /* only in kernel mode */
    SYSCALL, SYSRET, INT, IRET,
    END
  } inst;
  Symbol from, to;
  Time ts;
  size_t tid;
  size_t cpu;

  Action(): inst(END) {}
  bool operator==(const Action &that) const {
    return inst == that.inst && from == that.from && to == that.to &&
           ts == that.ts && tid == that.tid && cpu == that.cpu;
  }
  bool operator!=(const Action &that) const { return !(*this == that); }
};

struct GetAction {
  virtual ~GetAction() {}
  virtual Action next_action() = 0;
  virtual void stop() {};
};

class TraceReader : public GetAction {
protected:
  const static std::vector<std::pair<const std::string, Action::Inst>> str2inst;
  static Action next_action_for_stream(std::istream &);
  static Action get_action_from_line(std::string &);
};

class BasicReader : public TraceReader {
protected:
  std::istream *is;
public:
  BasicReader(std::istream *is): is(is) {}
  virtual Action next_action() { return next_action_for_stream(*is); }
};

/* reads files until EOF in sequence, suitable for file(s) */
class FileReader : public TraceReader {
  std::queue<std::istream *> iss;
public:
  FileReader(std::string f) { iss.push(new std::ifstream(f)); }
  FileReader(std::vector<std::string> &fs) {
    for (auto &f: fs) iss.push(new std::ifstream(f));
  }
  virtual Action next_action() {
    while (!iss.empty()) {
      auto a = next_action_for_stream(*iss.front());
      if (a.inst != Action::END) return a;
      iss.pop();
    }
    return Action();
  }
  virtual ~FileReader() {
    while (!iss.empty()) {
      delete iss.front();
      iss.pop();
    }
  }
};

/* reads streams until EOF in parallel, suitable for non-seekable stream */
class StreamReader : public TraceReader {
  size_t step;
  Time last = 0;

  std::vector<std::thread> thrs;
  struct Stream {
    bool from_file = false;
    std::istream *is;
    std::mutex lock;
    std::condition_variable empty;
    std::atomic<bool> finished{false};
    std::queue<std::queue<Action>> segments;
    Stream(std::istream *is): is(is) {}
    Stream(std::string &f): from_file(true), is(new std::ifstream(f)) {}
    ~Stream() { if (from_file) delete is; }
  };

  std::vector<Stream *> streams;
  std::atomic<bool> stop{false};
  void worker(size_t);

  std::queue<Action> current_segment;
  size_t current_stream = 0;
public:
  StreamReader(std::vector<std::string> &fs, size_t parallel, size_t step):
    step(step) {
    for (auto &f: fs) streams.push_back(new Stream(f));
    for (size_t i = 0; i < parallel; ++i)
      thrs.push_back(std::thread(&StreamReader::worker, this, i));
  }
  StreamReader(std::istream *is, size_t step): step(step) {
    streams.push_back(new Stream(is));
    thrs.push_back(std::thread(&StreamReader::worker, this, 0));
  }

  virtual ~StreamReader() {
    stop.store(true);
    for (auto &t: thrs) t.join();
    for (auto s: streams) delete s;
  }
  virtual Action next_action();
};

/* parse single file in parallel, suitable for large file */
class ParallelReader : public TraceReader {
  std::string file_name;
  size_t workers;
  struct JobQueue {
    std::thread thr;
    struct Job {
      long pos;
      long end_pos;
    };
    std::queue<Job> jobs;
    std::queue<std::queue<Action>> actions;
    std::mutex lock;
    std::condition_variable job_empty;
    std::condition_variable action_empty;
  };

  std::vector<JobQueue *> jqs;
  std::atomic<bool> stop{false};
  void worker(JobQueue *);

  std::queue<Action> current_block_of_action;
  size_t total_segment{0};
  size_t next_segment{0};

public:
  ParallelReader(std::string, size_t, size_t);
  virtual ~ParallelReader();
  virtual Action next_action();
};

class MergeWrapper : public GetAction {
  bool single_source = false;
  GetAction *tr;
  Time last = 0;

  struct ActionWrapper {
    Action act;
    GetAction *tr;
    bool operator<(const ActionWrapper &that) const {
      return act.ts > that.act.ts;
    }
  };

  std::queue<Action> block;
  std::priority_queue<ActionWrapper> action_heap;
public:
  MergeWrapper(std::vector<GetAction *> trs) {
    /* do a single read from all files to populate action_heap */
    if (trs.size() == 1) {
      single_source = true;
      tr = trs[0];
    } else for (auto tr: trs) action_heap.push({tr->next_action(), tr});
  }

  virtual Action next_action() {
    if (single_source) return tr->next_action();
    while (!action_heap.empty()) {
      auto act = action_heap.top();
      action_heap.pop();
      if (act.act.inst == Action::END) continue;
      action_heap.push({std::move(act.tr->next_action()), act.tr});
      return act.act;
    }
    return Action();
  }

  Action next_action_by_block() {
    if (single_source) return tr->next_action();
    if (!block.empty()) {
      auto ret = block.front();
      block.pop();
      return ret;
    }

    while (!action_heap.empty()) {
      auto act = action_heap.top();
      action_heap.pop();
      if (act.act.inst == Action::END) continue;
      auto next = act.tr->next_action();
      while (next.inst != Action::END && next.tid == act.act.tid) {
        block.push(std::move(next));
        next = act.tr->next_action();
      }
      if (next.inst != Action::END)
        action_heap.push({std::move(next), act.tr});
      return act.act;
    }
    return Action();
  }
};

#endif
