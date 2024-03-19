#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include "reader.hpp"

static const auto NS_IN_SEC = 1000000000UL;
static Time make_time(uint64_t s, uint64_t ns) { return s * NS_IN_SEC + ns; }

std::string pretty_time(Time t) {
  std::ostringstream str;
  if (t >= NS_IN_SEC)
    str << t / NS_IN_SEC << '.' << std::setfill('0') << std::setw(9);
  str << t % NS_IN_SEC;
  return str.str();
}

static std::string process_symbol(std::string str) {
#ifdef DO_SYMBOL_PROCESS
  /* remove trailing $plt, @plt
     e.g pthread_mutex_lock$plt <-> pthread_mutex_lock
     ceil@plt <-> __ceil_sse41 */
  if (str.substr(str.size() - 3) == "plt" &&
      (str[str.size() - 4] == '@' || str[str.size() - 4] == '$'))
    str = str.substr(0, str.size() - 4);
  /* remove trailing @@GLIBC_XXX
     e.g pthread_cond_timedwait <-> pthread_cond_timedwait@@GLIBC_2.3.2
   */
  size_t pos = str.rfind("@@GLIBC_");
  if (pos != std::string::npos) str = str.substr(0, pos);
  /* remove __ prefix */
  if (str.substr(0, 2) == "__") str = str.substr(2);
  /* remove __libc_ prefix */
  if (str.substr(0, 5) == "libc_") str = str.substr(5);

  /* simplify library functions
     e.g. __memset__SSE_4 -> memset */
  static const std::set<std::string> special_symbols = {
    "memcpy", "memcmp", "memset", "strcpy", "strcmp", "strlen", "ceil", "strcasecmp"
  };
  for (auto &ss: special_symbols) {
    if (str.find(ss) == std::string::npos) continue;
    str = ss;
    break;
  }
  /* normalize _L_XXX_YYY to pthread_mutex_XXX */
  if (str.substr(0, 3) == "_L_") {
    auto pos = str.find_last_of('_');
    str = "pthread_mutex_" + str.substr(3, pos - 3);
  }
#endif
  return str;
}

static Symbol get_symbol(std::string &str, size_t pos) {
  auto end = str.find_first_of(' ', pos);
  auto address = std::stoull(str.substr(pos, end - pos), nullptr, 16);
  pos = str.find_first_not_of(' ', end);
  static std::string unknown_symbol = "[unknown]";
  std::string symbol;
  if (str.substr(pos, unknown_symbol.size()) == unknown_symbol) {
    return {unknown_symbol, address, 0};
  } else {
    size_t end = str.find("+0x", pos);
    auto name = process_symbol(str.substr(pos, end - pos));
    pos = end + 1;
    end = str.find_first_of(' ', pos);
    auto offset = std::stoull(str.substr(pos, end - pos), nullptr, 16);
    return {name, address, offset};
  }
};

const std::vector<std::pair<const std::string, Action::Inst>> TraceReader::str2inst{
    {"call", Action::CALL},
    {"return", Action::RET},
    {"jmp", Action::JMP},
    {"jcc", Action::JCC},
    {"tr strt", Action::TR_START},
    {"tr end  syscall", Action::TR_END_SYSCALL},
    {"tr end", Action::TR_END},
    {"syscall", Action::SYSCALL},
    {"sysret", Action::SYSRET},
    {"hw int", Action::INT},
    {"iret", Action::IRET}
};

Action TraceReader::next_action_for_stream(std::istream &is) {
  std::string line;
  while (std::getline(is, line)) {
    Action action;
    while (1) {
      try {
        action = get_action_from_line(line);
        break;
      } catch (...) {
        std::cerr << "Error when reading line " << line << std::endl << std::flush;
        if (!std::getline(is, line)) return Action();
      }
    }
    /* filter redundant jmp */
    if ((action.inst == Action::JMP || action.inst == Action::JCC) &&
        (action.from.base() == action.to.base() ||
         action.from.name == action.to.name))
      continue;
    if (action.tid == 0) continue;
    return action;
  }
  return Action();
}

Action TraceReader::get_action_from_line(std::string &line) {
  Action act;
  /* typical output line from
     perf script --itrace=cr --ns -F-event,-period,+addr,-comm,+flags
     looks like:
     TID [CPU] SEC.NSEC: ACT ADDR FUNC+OFF (BIN) => ADDR FUNC+OFF (BIN)
     if FUNC is [unknown], +OFF is omitted
     with -F-dso, (BIN) is ommited
   */

  /* thread info */
  size_t start = 0;
  size_t end = line.find_first_of('[');
  act.tid = std::stol(line.substr(start, end - start));

  start = end + 1;
  end = line.find_first_of(']', start);
  act.cpu = std::stol(line.substr(start, end - start));

  /* fake tid for sched process on each cpu */
  // if (act.tid == 0) act.tid = UINT64_MAX - act.cpu;

  start = end + 1;
  end = line.find_first_of('.');
  size_t ts1 = std::stol(line.substr(start, end - start));
  start = end + 1;
  end = line.find_first_of(':');
  size_t ts2 = std::stol(line.substr(start, end - start));
  act.ts = make_time(ts1, ts2);

  start = line.find_first_not_of(' ', end + 1);
  end = std::string::npos;
  for (auto &[str, inst]: str2inst) {
    if (line.substr(start, str.size()) != str) continue;
    end = start + str.size();
    act.inst = inst;
    break;
  }
  if (end == std::string::npos) {
    std::cerr << "Trace line not matched" << std::endl << std::flush
              << line << std::endl << std::endl;
    throw 0;
  }

  start = line.find_first_not_of(' ', end);
  if (act.inst == Action::TR_END) {
    /* potential TR END  XXX instructions */
    end = line.find_first_of(' ', start);
    try {
      /* try converting the next substring to hex number */
      std::stoull(line.substr(start, end - start - 1), nullptr, 16);
    } catch (std::invalid_argument) {
      /* conversion failed, this is probably an unknown TR END  XXX instruction
         e.g. TR END  RETURN, pretend it is simply TR END */
      start = line.find_first_not_of(' ', end);
      std::cerr << "Unknown TR END: " << std::endl
                << line << std::endl << std::flush;
    }
  }
  act.from = get_symbol(line, start);
  start = line.find("=>", start);
  start = line.find_first_not_of(' ', start + 2);
  act.to = get_symbol(line, start);

  return act;
}

void StreamReader::worker(size_t idx) {
  for (auto i = idx; i < streams.size(); i += thrs.size()) {
    auto &s = *streams[i];
    while (!stop.load() && s.is->good()) {
      std::queue<Action> segment;
      size_t counter = 0;
      while (!stop.load() && s.is->good() && counter++ < step) {
        auto action = next_action_for_stream(*(s.is));
        if (action.inst != Action::END) segment.push(action);
      }
      if (segment.empty()) continue;

      s.lock.lock();
      s.segments.push(std::move(segment));
      s.lock.unlock();
      s.empty.notify_one();
    }
    s.finished.store(true);
    s.empty.notify_one();
  }
}

Action StreamReader::next_action() {
  while (current_segment.empty() && current_stream < streams.size()) {
    auto &s = *streams[current_stream];
    if (!s.finished.load()) {
      std::unique_lock<std::mutex> ul(s.lock);
      s.empty.wait(ul, [&s](){
        return !s.segments.empty() || s.finished.load();
      });
      if (s.segments.empty()) {
        current_stream++;
        continue;
      }
    } else {
      /* skip lock if worker is finished  */
      if (s.segments.empty()) {
        current_stream++;
        continue;
      }
    }
    current_segment = std::move(s.segments.front());
    s.segments.pop();
  }
  if (current_segment.empty()) return Action();

  auto ret = std::move(current_segment.front());
  current_segment.pop();
  last = ret.ts;
  return ret;
}

ParallelReader::ParallelReader(
    std::string file_name, size_t workers, size_t seek_step)
: file_name(file_name), workers(workers) {
  for (size_t i = 0; i < workers; ++i) {
    jqs.push_back(new JobQueue());
    jqs[i]->thr = std::thread(&ParallelReader::worker, this, jqs[i]);
    pthread_setname_np(jqs[i]->thr.native_handle(), "Reader");
  }

  std::ifstream file(file_name);
  bool reach_end = false;
  long pos = 0;
  while (!reach_end) {
    /* seek forward by seek step, then align pos to line break */
    file.seekg(seek_step, file.cur);
    std::string line;
    std::getline(file, line);
    if (!file.good()) {
      file.clear();
      file.seekg(0, file.end);
      reach_end = true;
    }
    auto next_pos = file.tellg();
    auto &jq = *jqs[total_segment++ % workers];
    jq.lock.lock();
    jq.jobs.push({pos, next_pos});
    jq.lock.unlock();
    jq.job_empty.notify_one();
    pos = next_pos;
  }
}

ParallelReader::~ParallelReader() {
  stop.store(true);
  for (auto jq: jqs) {
    jq->job_empty.notify_one();
    jq->thr.join();
    delete jq;
  }
}

void ParallelReader::worker(JobQueue *jq) {
  std::ifstream file(file_name);
  while (!stop.load()) {
    std::unique_lock<std::mutex> ul(jq->lock);
    jq->job_empty.wait(ul, [&]() { return !jq->jobs.empty() || stop.load(); });
    if (jq->jobs.empty()) return;
    auto job = jq->jobs.front();
    jq->jobs.pop();
    ul.unlock();
    file.seekg(job.pos);

    std::queue<Action> segment;
    while (file.good() && file.tellg() < job.end_pos) {
      auto a = next_action_for_stream(file);
      if (file.good() && file.tellg() > job.end_pos) break;
      if (a.inst == Action::END) break;
      segment.push(a);
    }

    ul.lock();
    jq->actions.push(segment);
    ul.unlock();
    jq->action_empty.notify_one();
  }
}

Action ParallelReader::next_action() {
  while (current_block_of_action.empty() && next_segment < total_segment) {
    auto jq = jqs[next_segment++ % workers];
    std::unique_lock<std::mutex> ul(jq->lock);
    jq->action_empty.wait(ul, [&](){ return !jq->actions.empty(); });
    current_block_of_action = std::move(jq->actions.front());
    jq->actions.pop();
  }
  if (current_block_of_action.empty()) return Action();

  auto ret = current_block_of_action.front();
  current_block_of_action.pop();
  return ret;
}
