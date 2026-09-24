/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __PANEL_REMOTE_MEMORY_HH__
#define __PANEL_REMOTE_MEMORY_HH__

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "astra-sim/system/AstraRemoteMemoryAPI.hh"
#include "astra-sim/system/Callable.hh"
#include "astra-sim/system/Sys.hh"

// The panel backend's copy of astra-sim's PanelRemoteMemory
// (extern/remote_memory_backend/analytical, an upstream submodule this project
// does not publish) with one fix: the backend's follow-up event owns its own
// record instead of the workload's handler data, which Workload::call deletes
// when the node completes (use-after-free; SIGSEGV/SIGBUS in call() with 64
// ranks queueing memory nodes -- both deaths of the Qwen3 TP64 serving runs).
namespace HTSim {

class MemoryCallData : public AstraSim::CallData {
 public:
  explicit MemoryCallData(int sys_id) : sys_id(sys_id) {}
  int sys_id;
};

enum MemoryArchitectureType {
  NO_MEMORY_EXPANSION = 0,
  PER_NODE_MEMORY_EXPANSION,
  PER_NPU_MEMORY_EXPANSION,
  MEMORY_POOL
};

class PendingMemoryRequest {
 public:
  PendingMemoryRequest(
      uint64_t tensor_size,
      AstraSim::WorkloadLayerHandlerData* wlhd)
    : tensor_size(tensor_size), wlhd(wlhd) {
  }

  uint64_t tensor_size;
  AstraSim::WorkloadLayerHandlerData* wlhd;
};

class PanelRemoteMemory : public AstraSim::AstraRemoteMemoryAPI, public AstraSim::Callable{
 public:
  PanelRemoteMemory(std::string memory_configuration) noexcept;
  void set_sys(int id, AstraSim::Sys* sys);
  void issue(
      uint64_t tensor_size,
      AstraSim::WorkloadLayerHandlerData* wlhd);
  void call(AstraSim::EventType type, AstraSim::CallData* data);
  uint64_t get_remote_mem_runtime(uint64_t tensor_size);

 private:
  MemoryArchitectureType mem_type;
  uint64_t remote_mem_latency; // remote memory access latency in nanosec
  uint64_t remote_mem_bw; // remote memory bandwidth in GB/sec
  std::vector<bool> ongoing_transaction;

  // per-node memory expansion
  int num_nodes;
  int num_npus_per_node;

  std::unordered_map<int, AstraSim::Sys*> sys_map;
  std::vector<std::deque<PendingMemoryRequest>> pending_requests;
};
} // namespace HTSim

#endif /* __PANEL_REMOTE_MEMORY_HH__ */
