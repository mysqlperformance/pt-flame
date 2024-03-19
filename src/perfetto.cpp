#include "perfetto.hpp"

Perfetto *perfetto = nullptr;
/* little endian */

void Perfetto::emit_header(RecordType t, uint16_t size,
                           uint64_t payload) {
  uint16_t type_size = (t & 0xf) | ((size & 0x0fff) << 4);
  os.write(reinterpret_cast<char *>(&type_size), 2);
  payload = payload & 0xffffffffffffUL;
  os.write(reinterpret_cast<char *>(&payload), 6);
}

void Perfetto::emit_event_header(uint16_t size, EventType t,
                                 size_t arg, size_t tid, size_t pid,
                                 const std::string &cat,
                                 const std::string &str) {
  uint64_t tid_index = register_thread(tid, pid);
  uint64_t cat_index = register_string(cat);
  uint64_t str_index = register_string(str);
  uint64_t payload = (t & 0xf) | ((arg & 0xf) << 4) |
                     ((tid_index & 0xff) << 8) | (cat_index << 16) |
                     (str_index << 32);
  emit_header(RecordType::EVENT, size, payload);
}

uint16_t Perfetto::register_string(const std::string &str) {
  /* string already in registry */
  if (strings.find(str) != strings.end())
    return strings[str];

  /* replace existing string */
  if (rstrings.find(string_index) != rstrings.end())
    strings.erase(rstrings[string_index]);
  rstrings[string_index] = str;
  strings[str] = string_index;

  uint64_t payload = (string_index & 0xffff) | ((str.length() & 0xffff) << 16);
  /* pad string to 8 byte alignment */
  size_t real_len = ((str.length() + 7) / 8);
  emit_header(RecordType::STRING, real_len + 1, payload);
  os.write(str.c_str(), str.length());
  char padding[9] = "\0\0\0\0\0\0\0\0";
  os.write(padding, real_len * 8 - str.length());

  auto ret = string_index;
  string_index++;
  if (string_index == 32768)
    string_index = 1;
  return ret;
}

uint16_t Perfetto::register_thread(size_t tid, size_t pid) {
  /* thread already in registry */
  if (threads.find(tid) != threads.end())
    return threads[tid];

  /* replace existing thread */
  if (rthreads.find(thread_index) != rthreads.end())
    threads.erase(rthreads[thread_index]);
  rthreads[thread_index] = tid;
  threads[tid] = thread_index;

  uint64_t payload = thread_index & 0xff;
  /* pad string to 8 byte alignment */
  emit_header(RecordType::THREAD, 3, payload);
  os.write(reinterpret_cast<char *>(&pid), 8);
  os.write(reinterpret_cast<char *>(&tid), 8);

  auto ret = thread_index;
  thread_index++;
  if (thread_index == 255)
    thread_index = 1;
  return ret;
}

void Perfetto::emit_magic() {
  uint64_t magic = 0x0016547846040010UL;
  os.write(reinterpret_cast<char *>(&magic), 8);
}

void Perfetto::emit_function(size_t tid, size_t pid, const std::string &name,
                             uint64_t time, EventType t, uint64_t end) {
  static const std::string category = "Function Call";
  size_t size = t == EventType::COMPLETE ? 3 : 2;
  emit_event_header(size, t, 0, tid, pid, category, name);
  os.write(reinterpret_cast<char *>(&time), 8);
  if (t == EventType::COMPLETE) os.write(reinterpret_cast<char *>(&end), 8);
}

// int main() {
//   std::ofstream of("p.out", std::ios::out | std::ios::binary);
//   Perfetto p;
//   std::string f1 = "f1";
//   std::string f2 = "f2";
//   std::string f3 = "f3";
//   std::string f4 = "f4";
//   p.emit_magic(of);
//   p.emit_function(of, 6, 16, f1, 0, Perfetto::EventType::BEGIN);
//   p.emit_function(of, 1, 2, f1, 1, Perfetto::EventType::BEGIN);
//   p.emit_function(of, 1, 2, f1, 6, Perfetto::EventType::END);
//   p.emit_function(of, 1, 2, f2, 4, Perfetto::EventType::BEGIN);
//   p.emit_function(of, 1, 2, f2, 5, Perfetto::EventType::END);
//   p.emit_function(of, 6, 16, f1, 2, Perfetto::EventType::END);
//   p.emit_function(of, 1, 2, f3, 2, Perfetto::EventType::COMPLETE, 5);
//   p.emit_function(of, 1, 2, f3, 1, Perfetto::EventType::COMPLETE, 7);
// }
