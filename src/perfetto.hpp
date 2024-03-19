#ifndef __PERFETTO_HEADER__
#define __PERFETTO_HEADER__

#include <string>
#include <fstream>
#include <unordered_map>

class Perfetto {
  std::unordered_map<std::string, size_t> strings;
  std::unordered_map<size_t, std::string> rstrings;
  uint16_t string_index = 1;
  std::unordered_map<size_t, size_t> threads;
  std::unordered_map<size_t, size_t> rthreads;
  uint16_t thread_index = 1;

  std::ofstream os;

public:
  enum RecordType {
    STRING = 2, THREAD, EVENT
  };
  enum EventType {
    INSTANT = 0, COUNTER, BEGIN, END, COMPLETE
  };
private:
  void emit_header(RecordType, uint16_t, uint64_t);
  void emit_event_header(uint16_t, EventType, size_t, size_t, size_t,
                         const std::string &, const std::string &);

  uint16_t register_string(const std::string &);
  uint16_t register_thread(size_t, size_t);
public:
  Perfetto(std::string f) : os(f) {}
  void emit_magic();
  void emit_function(size_t, size_t, const std::string &, uint64_t, EventType,
                     uint64_t = 0);
};

extern Perfetto *perfetto;

#endif
