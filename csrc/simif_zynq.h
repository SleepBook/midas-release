#ifndef __SIMIF_ZYNQ_H
#define __SIMIF_ZYNQ_H

#include "simif.h"

class simif_zynq_t : public simif_t
{
  public:
    simif_zynq_t(
      std::vector<std::string> args, 
      std::string prefix, 
      bool log = false);
    ~simif_zynq_t() { }

  private:
    virtual void poke_channel(size_t addr, uint64_t data);
    virtual uint64_t peek_channel(size_t addr);

    volatile uintptr_t* dev_vaddr;
    const static uintptr_t dev_paddr = 0x43C00000;
};

#endif // __SIMIF_ZYNQ_H