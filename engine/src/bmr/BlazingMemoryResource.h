#pragma once 

#include <rmm/device_memory_resource.hpp>
#include <atomic>

#include <sys/sysinfo.h>

class BlazingMemoryResource {
  virtual size_t get_used_memory_size() = 0 ;
  virtual size_t get_total_memory_size() = 0 ;
};

// /TODO copy cuda_memory_resource implemenation and add counters
class blazing_device_memory_resource : public device_memory_resource, BlazingMemoryResource {
  public:
    blazing_device_memory_resource() = default;
    virtual ~blazing_device_memory_resource() = default;

  private:
    void* do_allocate(std::size_t bytes, cudaStream_t stream) {
      if (used_memory + bytes < total_memory_size) {
        used_memory += bytes;
        return this->do_allocate(bytes, stream);
      }
    }

    void do_deallocate(void* p, std::size_t bytes, cudaStream_t stream) {
      if (used_memory < bytes) {
        used_memory = 0;
      } else {
        used_memory -= bytes;
      }
      return this->do_deallocate(p, bytes, stream);
    }


  size_t get_used_memory_size();
  size_t get_total_memory_size();

private:
  size_t total_memory_size;       // POOL_SIZE, GPU_DRIVER_MEM_INFO, .... 
  std::atomic<size_t> used_memory{0};    
};



class blazing_host_memory_mesource : BlazingMemoryResource{
  public:
    blazing_host_memory_mesource() = default;
    virtual ~blazing_host_memory_mesource() = default;

  // Compute the total RAM size in bytes
  size_t get_total_memory_size() {
    struct sysinfo si;
    sysinfo (&si);

    size_t total_ram = si.totalram;
    return total_ram;
  }

  //Compute the used RAM size in bytes
  size_t get_used_memory_size() {
    struct sysinfo si;
    sysinfo (&si);

    size_t total_ram = si.totalram;
    size_t free_ram = si.freeram
    return total_ram - free_ram;
  }
};


class blazing_disk_memory_resource : BlazingMemoryResource {
  public:
    blazing_host_memory_mesource() = default;
    virtual ~blazing_host_memory_mesource() = default;

  // TODO: api DISK TOTAL_MEMORY_SIZE(); kernel linux

  size_t get_used_memory_size();
  size_t get_total_memory_size();
};

