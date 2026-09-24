/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __WORKLOAD_HH__
#define __WORKLOAD_HH__

#include <memory>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "astra-sim/system/Callable.hh"
#include "astra-sim/system/CommunicatorGroup.hh"
#include "astra-sim/workload/HardwareResource.hh"
#include "extern/graph_frontend/chakra/src/feeder/et_feeder.h"

namespace AstraSim {

class Sys;
class DataSet;

class Workload : public Callable {
  public:
    Workload(Sys* sys,
             std::string et_filename,
             std::string comm_group_filename);
    ~Workload();

    // communicator groups
    void initialize_comm_group(std::string comm_group_filename);

    // event-based simulation
    void issue_dep_free_nodes();
    void issue(std::shared_ptr<Chakra::ETFeederNode> node);
    void issue_replay(std::shared_ptr<Chakra::ETFeederNode> node);
    void issue_remote_mem(std::shared_ptr<Chakra::ETFeederNode> node);
    void issue_comp(std::shared_ptr<Chakra::ETFeederNode> node);
    void issue_comm(std::shared_ptr<Chakra::ETFeederNode> node);
    void issue_workload_barrier(
        std::shared_ptr<Chakra::ETFeederNode> node);
    void issue_global_plan_round_barrier(
        std::shared_ptr<Chakra::ETFeederNode> node);
    void issue_plane_plan_configuration_barrier(
        std::shared_ptr<Chakra::ETFeederNode> node);
    void skip_invalid(std::shared_ptr<Chakra::ETFeederNode> node);
    void call(EventType event, CallData* data);
    void fire();

    // Serving mode: the frontend hands this rank one Chakra graph per
    // iteration over stdin, so a Workload outlives its first graph. Off by
    // default; the frontend enables it before any Sys is constructed. Outside
    // serving mode nothing below changes the existing single-graph behaviour.
    static void set_serving_mode(bool enabled);
    static bool serving_mode();
    // Unit of a Chakra COMP node's `duration_micros` when replayed: upstream
    // ASTRA-sim reads it as microseconds (the default here, and what every
    // retained panel workload was produced under); LLMServingSim writes
    // nanoseconds. Selected explicitly by the frontend, never inferred.
    static void set_runtime_unit_ns(bool ns);
    static bool runtime_unit_ns();
    // Queue (or, if this rank is idle, immediately start) the graph
    // `<new_filename>.<rank>.et` on this rank and on every rank in `systems`.
    void add_workload(const std::string& new_filename,
                      const std::vector<Sys*>& systems);
    // Mark this rank and every rank in `systems` idle until exit.
    void sleep_workload(const std::vector<Sys*>& systems);

    // stats
    void report();

    Chakra::ETFeeder* et_feeder;
    CommunicatorGroup* comm_group;
    HardwareResource* hw_resource;
    Sys* sys;
    std::unordered_map<int, uint64_t> collective_comm_node_id_map;
    std::unordered_map<int, DataSet*> collective_comm_wrapper_map;
    bool is_finished;
    // Serving-mode state. `iteration` counts graphs run on this rank and is
    // what the per-iteration report carries; `is_sleep` is set by "done".
    uint32_t iteration;
    bool is_sleep;
    std::queue<std::string> pending_workloads;

  private:
    void start_graph(const std::string& workload_filename);
    static bool serving_mode_;
    static bool runtime_unit_ns_;
};

}  // namespace AstraSim

#endif /* __WORKLOAD_HH__ */
