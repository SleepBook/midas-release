#include "debug_api.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <assert.h>
#include <iostream>
#include <fstream>
#include <bitset>
#include <stdlib.h>

#define read_reg(r) (dev_vaddr[r])
#define write_reg(r, v) (dev_vaddr[r] = v)

debug_api_t::debug_api_t(std::string design_, bool trace_)
  : design(design_), trace(trace_), t(0), snap_size(0), pass(true), fail_t(-1), input_num(0), output_num(0)
{
  int fd = open("/dev/mem", O_RDWR|O_SYNC);
  assert(fd != -1);

  int host_prot = PROT_READ | PROT_WRITE;
  int flags = MAP_SHARED;
  uintptr_t pgsize = sysconf(_SC_PAGESIZE);
  assert(dev_paddr % pgsize == 0);

  dev_vaddr = (uintptr_t*)mmap(0, pgsize, host_prot, flags, fd, dev_paddr);
  assert(dev_vaddr != MAP_FAILED);

  // Reset
  write_reg(31, 0); 
  __sync_synchronize();
  // Empty output queues before starting!
  while ((uint32_t) read_reg(0) > 0) {
    uint32_t temp = (uint32_t) read_reg(1);
  }

  // Read mapping files
  read_io_map_file(design + ".io.map");
  read_chain_map_file(design + ".chain.map");

  srand(time(NULL));

  replayfile = design + ".replay";
}

debug_api_t::~debug_api_t() {
  std::cout << t << " Cycles";
  if (pass) 
    std::cout << " Passed" << std::endl;
  else 
    std::cout << " Failed, first at cycle " << fail_t << std::endl;

  write_replay_file(replayfile);
}

void debug_api_t::read_io_map_file(std::string filename) {
  std::ifstream file(filename.c_str());
  std::string line;
  bool isInput = false;
  if (file) {
    while (getline(file, line)) {
      std::istringstream iss(line);
      std::string head;
      iss >> head;
           if (head == "HOSTLEN:") iss >> hostlen;
      else if (head == "ADDRLEN:") iss >> addrlen;
      else if (head == "MEMLEN:") iss >> memlen;
      else if (head == "CMDLEN:") iss >> cmdlen;
      else if (head == "STEP:") iss >> STEP;
      else if (head == "POKE:") iss >> POKE;
      else if (head == "PEEK:") iss >> PEEK;
      else if (head == "SNAP:") iss >> SNAP;
      else if (head == "MEM:") iss >> MEM;
      else if (head == "INPUT:") isInput = true;
      else if (head == "OUTPUT:") isInput = false;
      else {
        int width;
        iss >> width;
        if (isInput) {
          int n = (width - 1) / (hostlen - 1) + 1;
          input_map[head] = std::vector<int>();
          for (int i = 0 ; i < n ; i++) {
            input_map[head].push_back(input_num);
            input_num++;
          }
        } else {
          int n = (width - 1) / hostlen + 1;
          output_map[head] = std::vector<int>();
          for (int i = 0 ; i < n ; i++) {
            output_map[head].push_back(output_num);
            output_num++;
          }
        }
      }
    }
  } else {
    std::cout << "Cannot open " << filename << std::endl;
  }
  file.close();
}

void debug_api_t::read_chain_map_file(std::string filename) {
  std::ifstream file(filename.c_str());
  std::string line;
  if (file) {
    while(getline(file, line)) {
      std::istringstream iss(line);
      std::string path;
      int width;
      iss >> path >> width;
      signals.push_back(path);
      widths.push_back(width);
      snap_size += width;
    }
  } else {
    std::cout << "Cannot open " << filename << std::endl;
  }
  file.close();
}

void debug_api_t::write_replay_file(std::string filename) {
  std::ofstream file(filename.c_str());
  if (file) {
    file << replay.str();    
  } else {
    std::cout << "Cannot open " << filename << std::endl;
  }
  file.close();
}

void debug_api_t::poke(uint64_t value) {
  write_reg(0, value);
  __sync_synchronize();
}

uint64_t debug_api_t::peek() {
  __sync_synchronize();
  while ((uint32_t) read_reg(0) == 0); 
  return (uint32_t) read_reg(1);
}

void debug_api_t::poke_all() {
  poke(POKE);
  for (int i = 0 ; i < input_num ; i++) {
    if (poke_map.find(i) != poke_map.end()) {
      poke(poke_map[i] << 1 | 1);
    } else {
      poke(0);
    }
  }
  poke_map.clear();
}

void debug_api_t::peek_all() {
  peek_map.clear();
  poke(PEEK);
  for (int i = 0 ; i < output_num ; i++) {
    peek_map[i] = peek();
  }
}

void debug_api_t::read_snap(std::string &snap) {
// void debug_api_t::read_snap(std::ostringstream &snap) {
  for (int i = 0 ; i < snap_size / hostlen ; i++) {
    uint32_t value = peek();
    std::bitset<sizeof(uint32_t)*8> bin_value(value);
    snap += bin_value.to_string();
  }
}

void debug_api_t::write_snap(std::string &snap, uint32_t n) {
  static bool begin = false;
  if (begin) {
    replay << "STEP " << n << std::endl;
    for (int i = 0 ; i < outputs.size() ; i++) {
      std::string output = outputs[i];
      replay << "EXPECT " << output << " " << peek(output) << std::endl;
    }
  }

  // Translate and write snapshots
  int offset = 0;
  for (int i = 0 ; i < signals.size() ; i++) {
    std::string signal = signals[i];
    int width = widths[i];
    if (signal != "null") {
      std::bitset<512> value(snap.substr(offset, width));
      replay << "POKE " << signal << " " << value.to_ulong() << std::endl;
    }
    offset += width;
  }
  begin = true;
}

void debug_api_t::poke_snap() {
  poke(SNAP);
}

void debug_api_t::poke_steps(uint32_t n) {
  poke(n << cmdlen | STEP);
}

void debug_api_t::step(uint32_t n) {
  std::string snap = "";
  poke_all();
  if (t > 0) poke_snap();
  poke_steps(n);
  if (t > 0) read_snap(snap);
  peek_all();
  if (trace) std::cout << "* STEP " << n << " -> " << (t + n) << " * " << std::endl;
  if (t > 0) write_snap(snap, n);
  t += n;
}

void debug_api_t::poke(std::string path, uint64_t value) {
  assert(input_map.find(path) != input_map.end());
  if (trace) std::cout << "* POKE " << path << " <- " << value << " * " << std::endl;
  std::vector<int> ids = input_map[path];
  uint64_t mask = (1 << (hostlen-1)) - 1;
  for (int i = 0 ; i < ids.size() ; i++) {
    int id = ids[ids.size()-1-i];
    int shift = (hostlen-1) * i;
    uint32_t data = (value >> shift) & mask;
    poke_map[id] = data;
  }
}

uint64_t debug_api_t::peek(std::string path) {
  assert(output_map.find(path) != output_map.end());
  uint64_t value = 0;
  std::vector<int> ids = output_map[path];
  for (int i = 0 ; i < ids.size() ; i++) {
    int id = ids[ids.size()-1-i];
    assert(peek_map.find(id) != peek_map.end());
    value = value << hostlen | peek_map[id];
  }
  if (trace) std::cout << "* PEEK " << path << " -> " << value << " * " << std::endl;
  return value;
}

bool debug_api_t::expect(std::string path, uint64_t expected) {
  int value = peek(path);
  bool ok = value == expected;
  pass &= ok;
  if (trace) {
    std::cout << "* EXPECT " << path << " -> " << value << " == " << expected;
    if (ok) {
      std::cout << " PASS * " << std::endl;
    }  else {
      if (fail_t < 0) fail_t = t;
      std::cout << " FAIL * " << std::endl;
    }
  }
  return ok;
}

bool debug_api_t::expect(bool ok, std::string s) {
  pass &= ok;
  if (trace) {
    std::cout << "* " << s;
    if (ok) {
      std::cout << " PASS * " << std::endl;
    } else {
      if (fail_t < 0) fail_t = t;
      std::cout << " FAIL * " << std::endl;
    }
  }
  return ok;
}

void debug_api_t::load_mem(std::string filename) {
  std::ifstream in(filename.c_str());
  if (!in) {
    std::cerr << "cound not open " << filename << std::endl;
    exit(1);
  }
  
  std::string line;
  int i = 0;
  while (std::getline(in, line)) {
    #define parse_nibble(c) ((c) >= 'a' ? (c)-'a'+10 : (c)-'0')
    uint64_t base = (i * line.length()) / 2;
    uint64_t offset = 0;
    for (int k = line.length() - 8 ; k >= 0 ; k -= 8) {
      uint64_t addr = base + offset;
      uint64_t data = 
        (parse_nibble(line[k]) << 28) | (parse_nibble(line[k+1]) << 24) |
        (parse_nibble(line[k+2]) << 20) | (parse_nibble(line[k+3]) << 16) |
        (parse_nibble(line[k+4]) << 12) | (parse_nibble(line[k+5]) << 8) |
        (parse_nibble(line[k+6]) << 4) | parse_nibble(line[k+7]);
      write_mem(base + offset, data);
      offset += 4;
    }
    i += 1;
  }
}

void debug_api_t::write_mem(uint64_t addr, uint64_t data) {
  poke((1 << cmdlen) | MEM);
  uint64_t mask = (1<<hostlen)-1;
  for (int i = (addrlen-1)/hostlen+1 ; i > 0 ; i--) {
    poke((addr >> (hostlen * (i-1))) & mask);
  }
  for (int i = (memlen-1)/hostlen+1 ; i > 0 ; i--) {
    poke((data >> (hostlen * (i-1))) & mask);
  }
}

uint64_t debug_api_t::read_mem(uint64_t addr) {
  poke((0 << cmdlen) | MEM);
  uint64_t mask = (1<<hostlen)-1;
  for (int i = (addrlen-1)/hostlen+1 ; i > 0 ; i--) {
    poke((addr >> (hostlen * (i-1))) & mask);
  }
  uint64_t data = 0;
  for (int i = 0 ; i < (memlen-1)/hostlen+1 ; i ++) {
    data |= peek() << (hostlen * i);
  }
  return data;
}

uint64_t debug_api_t::rand_next(int limit) {
  return rand() % limit;
}