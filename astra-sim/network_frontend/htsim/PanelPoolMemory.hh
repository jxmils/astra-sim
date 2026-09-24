#pragma once
// A physical memory pool for the panel backend (serving integration, G5).
//
// The pool is a device of the fabric graph: an endpoint P reachable over its
// attachment links and, behind it, a bank B reached only over P->B / B->P at
// the bank's service rate and controller latency (compose_fabric.py writes
// the graph; this file reads the matching --memory-pool-configuration JSON:
//   {"pools": [{"id": "pool0", "endpoint": 6, "bank": 7, ...}],
//    "tensor_loc_pool": {"CXL": "pool0"}}).
// A Chakra MEM_LOAD from a pool is one htsim flow B -> rank whose completion
// (last byte at the rank) completes the node: nothing reads before arrival.
// A MEM_STORE is one flow rank -> B completed by the sender's final ack. The
// pool transfer is lowered exactly once (never an analytical delay *and* a
// flow). Locations not mapped to a pool (REMOTE = CPU memory) keep the
// analytical remote-memory behaviour, so runs without a pool are unchanged.
#include <cstdint>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "astra-sim/system/AstraRemoteMemoryAPI.hh"
#include "astra-sim/system/Sys.hh"

namespace HTSim {

class PanelPoolMemory : public AstraSim::AstraRemoteMemoryAPI {
  public:
    struct Pool {
        std::string id;
        int endpoint = -1;
        int bank = -1;
        uint64_t capacity_bytes = 0;
        uint64_t loads = 0, stores = 0, load_bytes = 0, store_bytes = 0;
        uint64_t in_flight = 0;
    };
    PanelPoolMemory(const std::string& config_path,
                    std::unique_ptr<AstraSim::AstraRemoteMemoryAPI> fallback);
    void set_sys(int id, AstraSim::Sys* sys) override;
    void issue(uint64_t tensor_size, AstraSim::WorkloadLayerHandlerData* wlhd) override;
    // "POOL_STATS ..." per pool, once, at the end of the run.
    void report(std::ostream& os) const;
    const std::vector<Pool>& pools() const { return pools_; }

  private:
    struct Transfer;
    static void transfer_done(void* arg);
    static void ignore(void* arg);
    int pool_for(uint32_t tensor_loc, uint32_t tensor_device) const;

    std::vector<Pool> pools_;
    std::map<uint32_t, int> loc_to_pool_;   // Chakra tensor_loc -> index in pools_
    std::unique_ptr<AstraSim::AstraRemoteMemoryAPI> fallback_;
    std::map<int, AstraSim::Sys*> sys_;
    int next_tag_;
    uint64_t fallback_issues_ = 0;
};

}  // namespace HTSim
