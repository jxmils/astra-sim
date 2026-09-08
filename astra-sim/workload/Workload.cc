/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/workload/Workload.hh"

#include "astra-sim/common/Logging.hh"
#include "astra-sim/system/IntData.hh"
#include "astra-sim/system/MemEventHandlerData.hh"
#include "astra-sim/system/RecvPacketEventHandlerData.hh"
#include "astra-sim/system/SendPacketEventHandlerData.hh"
#include "astra-sim/system/WorkloadLayerHandlerData.hh"
#include <json/json.hpp>

#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <stdlib.h>
#include <tuple>
#include <unistd.h>

using namespace std;
using namespace AstraSim;
using namespace Chakra;
using json = nlohmann::json;

typedef ChakraProtoMsg::NodeType ChakraNodeType;
typedef ChakraProtoMsg::CollectiveCommType ChakraCollectiveCommType;

namespace {
struct GlobalPlanBarrierArrival {
    Workload* workload;
    uint64_t node_id;
    int rank;
};

struct WorkloadBarrierKey {
    uint64_t node_id;
    std::string name;
    std::vector<int> participants;

    bool operator<(const WorkloadBarrierKey& other) const {
        return std::tie(node_id, name, participants) <
               std::tie(other.node_id, other.name, other.participants);
    }
};

struct WorkloadBarrierRelease {
    WorkloadBarrierKey key;
    std::vector<GlobalPlanBarrierArrival> arrivals;
};

struct GlobalPlanBarrierRelease {
    int64_t completed_round;
    int64_t target_round;
    std::vector<GlobalPlanBarrierArrival> arrivals;
};

struct PlanePlanBarrierKey {
    int plane;
    int configuration;

    bool operator<(const PlanePlanBarrierKey& other) const {
        return std::tie(plane, configuration) <
               std::tie(other.plane, other.configuration);
    }
};

struct PlanePlanBarrierRelease {
    PlanePlanBarrierKey key;
    std::vector<GlobalPlanBarrierArrival> arrivals;
};

std::map<int64_t, std::vector<GlobalPlanBarrierArrival>>
    global_plan_barrier_arrivals;
std::map<WorkloadBarrierKey, std::vector<GlobalPlanBarrierArrival>>
    workload_barrier_arrivals;
std::map<PlanePlanBarrierKey, std::vector<GlobalPlanBarrierArrival>>
    plane_plan_barrier_arrivals;
int64_t next_global_plan_round = 0;
std::map<int, int> next_plane_plan_configuration;

constexpr const char* kPlanRoundBarrierPrefix = "ocs/global-round-";
constexpr const char* kPlanPlaneBarrierPrefix = "ocs/plane-";

bool has_reserved_plan_round_barrier_name(
    const shared_ptr<Chakra::ETFeederNode>& node) {
    return node->name().compare(
               0, std::char_traits<char>::length(kPlanRoundBarrierPrefix),
               kPlanRoundBarrierPrefix) == 0;
}

bool is_plan_round_barrier(const shared_ptr<Chakra::ETFeederNode>& node) {
    return node->has_other_attr("plan_global_round") ||
           has_reserved_plan_round_barrier_name(node);
}

bool has_reserved_plan_plane_barrier_name(
    const shared_ptr<Chakra::ETFeederNode>& node) {
    return node->name().compare(
               0, std::char_traits<char>::length(kPlanPlaneBarrierPrefix),
               kPlanPlaneBarrierPrefix) == 0;
}

bool is_plan_plane_barrier(const shared_ptr<Chakra::ETFeederNode>& node) {
    return node->has_other_attr("plan_plane") ||
           node->has_other_attr("plan_configuration") ||
           has_reserved_plan_plane_barrier_name(node);
}

[[noreturn]] void global_plan_barrier_fatal(const std::string& reason,
                                            int rank,
                                            int64_t round) {
    std::cerr << "PLAN_ROUND_BARRIER_FATAL"
              << " reason=" << reason
              << " rank=" << rank
              << " round=" << round << std::endl;
    std::exit(EXIT_FAILURE);
}

[[noreturn]] void workload_barrier_fatal(const std::string& reason,
                                         int rank,
                                         uint64_t node_id) {
    std::cerr << "WORKLOAD_BARRIER_FATAL"
              << " reason=" << reason
              << " rank=" << rank
              << " node_id=" << node_id << std::endl;
    std::exit(EXIT_FAILURE);
}

[[noreturn]] void plane_plan_barrier_fatal(const std::string& reason,
                                           int rank,
                                           int plane,
                                           int configuration) {
    std::cerr << "PLAN_PLANE_BARRIER_FATAL"
              << " reason=" << reason
              << " rank=" << rank
              << " plane=" << plane
              << " configuration=" << configuration << std::endl;
    std::exit(EXIT_FAILURE);
}

void release_workload_barrier(void* argument) {
    std::unique_ptr<WorkloadBarrierRelease> release(
        static_cast<WorkloadBarrierRelease*>(argument));
    std::cout << "WORKLOAD_BARRIER_RELEASE"
              << " node_id=" << release->key.node_id
              << " ranks=" << release->arrivals.size()
              << " tick=" << Sys::boostedTick() << std::endl;
    for (const auto& item : release->arrivals) {
        WorkloadLayerHandlerData* wlhd = new WorkloadLayerHandlerData;
        wlhd->sys_id = item.rank;
        wlhd->workload = item.workload;
        wlhd->node_id = item.node_id;
        item.workload->sys->register_event(
            item.workload, EventType::General, wlhd, 0);
    }
}

void release_global_plan_round_barrier(void* argument) {
    std::unique_ptr<GlobalPlanBarrierRelease> release(
        static_cast<GlobalPlanBarrierRelease*>(argument));
    if (release->completed_round != next_global_plan_round) {
        global_plan_barrier_fatal(
            "release_round_not_active", -1, release->completed_round);
    }
    ++next_global_plan_round;
    std::cout << "PLAN_ROUND_BARRIER_RELEASE"
              << " round=" << release->completed_round
              << " target_round=" << release->target_round
              << " ranks=" << release->arrivals.size()
              << " tick=" << Sys::boostedTick() << std::endl;
    for (const auto& item : release->arrivals) {
        WorkloadLayerHandlerData* wlhd = new WorkloadLayerHandlerData;
        wlhd->sys_id = item.rank;
        wlhd->workload = item.workload;
        wlhd->node_id = item.node_id;
        item.workload->sys->register_event(
            item.workload, EventType::General, wlhd, 0);
    }
}

void try_release_plane_plan_configuration(int plane);

void release_plane_plan_configuration_barrier(void* argument) {
    std::unique_ptr<PlanePlanBarrierRelease> release(
        static_cast<PlanePlanBarrierRelease*>(argument));
    const int expected = next_plane_plan_configuration[release->key.plane];
    if (release->key.configuration != expected) {
        plane_plan_barrier_fatal(
            "release_configuration_not_active", -1, release->key.plane,
            release->key.configuration);
    }
    ++next_plane_plan_configuration[release->key.plane];
    std::cout << "PLAN_PLANE_BARRIER_RELEASE"
              << " plane=" << release->key.plane
              << " configuration=" << release->key.configuration
              << " ranks=" << release->arrivals.size()
              << " tick=" << Sys::boostedTick() << std::endl;
    try_release_plane_plan_configuration(release->key.plane);
    for (const auto& item : release->arrivals) {
        WorkloadLayerHandlerData* wlhd = new WorkloadLayerHandlerData;
        wlhd->sys_id = item.rank;
        wlhd->workload = item.workload;
        wlhd->node_id = item.node_id;
        item.workload->sys->register_event(
            item.workload, EventType::General, wlhd, 0);
    }
}

void try_release_plane_plan_configuration(int plane) {
    const int configuration = next_plane_plan_configuration[plane];
    const PlanePlanBarrierKey key{plane, configuration};
    auto waiting = plane_plan_barrier_arrivals.find(key);
    if (waiting == plane_plan_barrier_arrivals.end() ||
        waiting->second.size() < Sys::all_sys.size()) {
        return;
    }
    if (waiting->second.size() != Sys::all_sys.size()) {
        plane_plan_barrier_fatal(
            "too_many_arrivals", -1, plane, configuration);
    }
    std::set<int> ranks;
    for (const auto& item : waiting->second) ranks.insert(item.rank);
    if (ranks.size() != Sys::all_sys.size()) {
        plane_plan_barrier_fatal(
            "rank_set_mismatch", -1, plane, configuration);
    }

    auto* release = new PlanePlanBarrierRelease{key, waiting->second};
    Workload* requester = waiting->second.front().workload;
    plane_plan_barrier_arrivals.erase(waiting);
    requester->sys->comm_NI->sim_wait_for_plan_configuration(
        plane, configuration,
        &release_plane_plan_configuration_barrier,
        release);
}
}  // namespace

Workload::Workload(Sys* sys, string et_filename, string comm_group_filename) {
    string workload_filename = et_filename + "." + to_string(sys->id) + ".et";
    // Check if workload filename exists
    if (access(workload_filename.c_str(), R_OK) < 0) {
        string error_msg;
        if (errno == ENOENT) {
            error_msg =
                "workload file: " + workload_filename + " does not exist";
        } else if (errno == EACCES) {
            error_msg = "workload file: " + workload_filename +
                        " exists but is not readable";
        } else {
            error_msg =
                "Unknown workload file: " + workload_filename + " access error";
        }
        LoggerFactory::get_logger("workload")->critical(error_msg);
        exit(EXIT_FAILURE);
    }
    this->et_feeder = new ETFeeder(workload_filename);
    this->comm_group = nullptr;
    // TODO: parametrize the number of available hardware resources
    this->hw_resource = new HardwareResource(1);
    this->sys = sys;
    initialize_comm_group(comm_group_filename);
    this->is_finished = false;
}

Workload::~Workload() {
    if (this->comm_group != nullptr) {
        delete this->comm_group;
    }
    if (this->et_feeder != nullptr) {
        delete this->et_feeder;
    }
    if (this->hw_resource != nullptr) {
        delete this->hw_resource;
    }
}

void Workload::initialize_comm_group(string comm_group_filename) {
    // communicator group input file is not given
    if (comm_group_filename.find("empty") != std::string::npos) {
        comm_group = nullptr;
        return;
    }

    ifstream inFile;
    json j;
    inFile.open(comm_group_filename);
    inFile >> j;

    for (json::iterator it = j.begin(); it != j.end(); ++it) {
        bool in_comm_group = false;

        for (auto id : it.value()) {
            if (id == sys->id) {
                in_comm_group = true;
            }
        }

        if (in_comm_group) {
            std::vector<int> involved_NPUs;
            for (auto id : it.value()) {
                involved_NPUs.push_back(id);
            }
            comm_group = new CommunicatorGroup(1, involved_NPUs, sys);
            // Note: All NPUs should create comm group with identical ids if
            // they want to communicate with each other
        }
    }
}

void Workload::issue_dep_free_nodes() {
    std::queue<shared_ptr<Chakra::ETFeederNode>> push_back_queue;
    shared_ptr<Chakra::ETFeederNode> node = et_feeder->getNextIssuableNode();
    while (node != nullptr) {
        if (hw_resource->is_available(node)) {
            issue(node);
        } else {
            push_back_queue.push(node);
        }
        node = et_feeder->getNextIssuableNode();
    }

    while (!push_back_queue.empty()) {
        shared_ptr<Chakra::ETFeederNode> node = push_back_queue.front();
        et_feeder->pushBackIssuableNode(node->id());
        push_back_queue.pop();
    }
}

void Workload::issue(shared_ptr<Chakra::ETFeederNode> node) {
    auto logger = LoggerFactory::get_logger("workload");
    if (sys->replay_only) {
        hw_resource->occupy(node);
        issue_replay(node);
    } else {
        if ((node->type() == ChakraNodeType::MEM_LOAD_NODE) ||
            (node->type() == ChakraNodeType::MEM_STORE_NODE)) {
            if (sys->trace_enabled) {
                logger->debug("issue,sys->id={}, tick={}, node->id={}, "
                              "node->name={}, node->type={}",
                              sys->id, Sys::boostedTick(), node->id(),
                              node->name(),
                              static_cast<uint64_t>(node->type()));
            }
            issue_remote_mem(node);
        } else if (node->is_cpu_op() ||
                   (!node->is_cpu_op() &&
                    node->type() == ChakraNodeType::COMP_NODE)) {
            if ((node->runtime() == 0) && (node->num_ops() == 0)) {
                skip_invalid(node);
            } else {
                if (sys->trace_enabled) {
                    logger->debug("issue,sys->id={}, tick={}, node->id={}, "
                                  "node->name={}, node->type={}",
                                  sys->id, Sys::boostedTick(), node->id(),
                                  node->name(),
                                  static_cast<uint64_t>(node->type()));
                }
                issue_comp(node);
            }
        } else if (!node->is_cpu_op() &&
                   (node->type() == ChakraNodeType::COMM_COLL_NODE ||
                    (node->type() == ChakraNodeType::COMM_SEND_NODE) ||
                    (node->type() == ChakraNodeType::COMM_RECV_NODE))) {
            if (sys->trace_enabled) {
                if (sys->trace_enabled) {
                    logger->debug("issue,sys->id={}, tick={}, node->id={}, "
                                  "node->name={}, node->type={}",
                                  sys->id, Sys::boostedTick(), node->id(),
                                  node->name(),
                                  static_cast<uint64_t>(node->type()));
                }
            }
            issue_comm(node);
        } else if (node->type() == ChakraNodeType::INVALID_NODE) {
            skip_invalid(node);
        }
    }
}

void Workload::issue_replay(shared_ptr<Chakra::ETFeederNode> node) {
    WorkloadLayerHandlerData* wlhd = new WorkloadLayerHandlerData;
    wlhd->node_id = node->id();
    uint64_t runtime = 1ul;
    if (node->runtime() != 0ul) {
        // chakra runtimes are in microseconds and we should convert it into
        // nanoseconds
        runtime = node->runtime() * 1000;
    }
    if (node->is_cpu_op()) {
        hw_resource->tics_cpu_ops += runtime;
    } else {
        hw_resource->tics_gpu_ops += runtime;
    }
    sys->register_event(this, EventType::General, wlhd, runtime);
}

void Workload::issue_remote_mem(shared_ptr<Chakra::ETFeederNode> node) {
    hw_resource->occupy(node);

    WorkloadLayerHandlerData* wlhd = new WorkloadLayerHandlerData;
    wlhd->sys_id = sys->id;
    wlhd->workload = this;
    wlhd->node_id = node->id();
    sys->remote_mem->issue(node->tensor_size(), wlhd);
}

void Workload::issue_comp(shared_ptr<Chakra::ETFeederNode> node) {
    hw_resource->occupy(node);

    if (sys->roofline_enabled) {
        WorkloadLayerHandlerData* wlhd = new WorkloadLayerHandlerData;
        wlhd->node_id = node->id();

        double operational_intensity = static_cast<double>(node->num_ops()) /
                                       static_cast<double>(node->tensor_size());
        double perf = sys->roofline->get_perf(operational_intensity);
        double elapsed_time =
            static_cast<double>(node->num_ops()) / perf;  // sec
        uint64_t runtime =
            static_cast<uint64_t>(elapsed_time * 1e9);  // sec -> ns
        if (node->is_cpu_op()) {
            hw_resource->tics_cpu_ops += runtime;
        } else {
            hw_resource->tics_gpu_ops += runtime;
        }
        sys->register_event(this, EventType::General, wlhd, runtime);
    } else {
        // advance this node forward the recorded "replayed" time specificed in
        // the ET.
        issue_replay(node);
    }
}

void Workload::issue_comm(shared_ptr<Chakra::ETFeederNode> node) {
    if (node->type() == ChakraNodeType::COMM_COLL_NODE &&
        node->comm_type() == ChakraCollectiveCommType::BARRIER) {
        const bool round_barrier = is_plan_round_barrier(node);
        const bool plane_barrier = is_plan_plane_barrier(node);
        if (round_barrier && plane_barrier) {
            global_plan_barrier_fatal("mixed_plan_barrier_modes", sys->id, -1);
        }
        if (round_barrier) {
            hw_resource->occupy(node);
            issue_global_plan_round_barrier(node);
        } else if (plane_barrier) {
            issue_plane_plan_configuration_barrier(node);
        } else {
            issue_workload_barrier(node);
        }
        return;
    }

    hw_resource->occupy(node);

    vector<bool> involved_dim;

    if (node->has_other_attr("involved_dim")) {
        const ChakraProtoMsg::AttributeProto& attr =
            node->get_other_attr("involved_dim");

        // Ensure the attribute is of type bool_list before accessing
        if (attr.has_bool_list()) {
            const ChakraProtoMsg::BoolList& bool_list = attr.bool_list();

            // Traverse bool_list and add values to involved_dim
            for (int i = 0; i < bool_list.values_size(); ++i) {
                involved_dim.push_back(bool_list.values(i));
            }
        } else {
            cerr << "Expected bool_list in involved_dim but found another type."
                 << endl;
            exit(EXIT_FAILURE);
        }
    } else {
        // involved_dim does not exist in ETFeeder. Keep the fallback large
        // enough for every logical topology supported by ASTRA-Sim; callers
        // index this vector once per topology dimension.
        // Could use Process Group to build involved_dim later.
        // Once process group is implemented, you should get
        // that with node->pg_name()

        involved_dim.assign(10, true);
    }

    if (!node->is_cpu_op() &&
        (node->type() == ChakraNodeType::COMM_COLL_NODE)) {
        if (node->comm_type() == ChakraCollectiveCommType::ALL_REDUCE) {
            DataSet* fp =
                sys->generate_all_reduce(node->comm_size(), involved_dim,
                                         comm_group, node->comm_priority());
            collective_comm_node_id_map[fp->my_id] = node->id();
            collective_comm_wrapper_map[fp->my_id] = fp;
            fp->set_notifier(this, EventType::CollectiveCommunicationFinished);

        } else if (node->comm_type() == ChakraCollectiveCommType::ALL_TO_ALL) {
            DataSet* fp =
                sys->generate_all_to_all(node->comm_size(), involved_dim,
                                         comm_group, node->comm_priority());
            collective_comm_node_id_map[fp->my_id] = node->id();
            collective_comm_wrapper_map[fp->my_id] = fp;
            fp->set_notifier(this, EventType::CollectiveCommunicationFinished);

        } else if (node->comm_type() == ChakraCollectiveCommType::ALL_GATHER) {
            DataSet* fp =
                sys->generate_all_gather(node->comm_size(), involved_dim,
                                         comm_group, node->comm_priority());
            collective_comm_node_id_map[fp->my_id] = node->id();
            collective_comm_wrapper_map[fp->my_id] = fp;
            fp->set_notifier(this, EventType::CollectiveCommunicationFinished);

        } else if (node->comm_type() ==
                   ChakraCollectiveCommType::REDUCE_SCATTER) {
            DataSet* fp =
                sys->generate_reduce_scatter(node->comm_size(), involved_dim,
                                             comm_group, node->comm_priority());
            collective_comm_node_id_map[fp->my_id] = node->id();
            collective_comm_wrapper_map[fp->my_id] = fp;
            fp->set_notifier(this, EventType::CollectiveCommunicationFinished);

        } else if (node->comm_type() == ChakraCollectiveCommType::BROADCAST) {
            // broadcast colelctive has not been implemented in ASTRA-SIM yet.
            // So, we just use its real system mesurements
            uint64_t runtime = 1ul;
            if (node->runtime() != 0ul) {
                // chakra runtimes are in microseconds and we should convert it
                // into nanoseconds
                runtime = node->runtime() * 1000;
            }
            DataSet* fp = new DataSet(1);
            fp->set_notifier(this, EventType::CollectiveCommunicationFinished);
            collective_comm_node_id_map[fp->my_id] = node->id();
            collective_comm_wrapper_map[fp->my_id] = fp;
            sys->register_event(fp, EventType::General, nullptr,
                                // chakra runtimes are in microseconds and we
                                // should convert it into nanoseconds
                                runtime);
            fp->set_notifier(this, EventType::CollectiveCommunicationFinished);
        }
    } else if (node->type() == ChakraNodeType::COMM_SEND_NODE) {
        sim_request snd_req;
        snd_req.srcRank = node->comm_src();
        snd_req.dstRank = node->comm_dst();
        snd_req.tag = node->comm_tag();
        snd_req.reqType = UINT8;
        SendPacketEventHandlerData* sehd = new SendPacketEventHandlerData;
        sehd->callable = this;
        sehd->wlhd = new WorkloadLayerHandlerData;
        sehd->wlhd->node_id = node->id();
        sehd->event = EventType::PacketSent;
        if (node->has_other_attr("flow_uid")) {
            const ChakraProtoMsg::AttributeProto& flow_uid =
                node->get_other_attr("flow_uid");
            if (!flow_uid.has_string_val() || flow_uid.string_val().empty()) {
                cerr << "flow_uid must be a nonempty string attribute" << endl;
                exit(EXIT_FAILURE);
            }
            snd_req.flow_uid = flow_uid.string_val();
        }
        sys->front_end_sim_send(0, Sys::dummy_data, node->comm_size(), UINT8,
                                node->comm_dst(), node->comm_tag(), &snd_req,
                                Sys::FrontEndSendRecvType::NATIVE,
                                &Sys::handleEvent, sehd);
    } else if (node->type() == ChakraNodeType::COMM_RECV_NODE) {
        sim_request rcv_req;
        RecvPacketEventHandlerData* rcehd = new RecvPacketEventHandlerData;
        rcehd->wlhd = new WorkloadLayerHandlerData;
        rcehd->wlhd->node_id = node->id();
        rcehd->workload = this;
        rcehd->event = EventType::PacketReceived;
        sys->front_end_sim_recv(0, Sys::dummy_data, node->comm_size(), UINT8,
                                node->comm_src(), node->comm_tag(), &rcv_req,
                                Sys::FrontEndSendRecvType::NATIVE,
                                &Sys::handleEvent, rcehd);
    } else {
        LoggerFactory::get_logger("workload")
            ->critical("Unknown communication node type");
        exit(EXIT_FAILURE);
    }
}

void Workload::issue_workload_barrier(
    shared_ptr<Chakra::ETFeederNode> node) {
    if (node->comm_size() != 0) {
        workload_barrier_fatal("nonzero_payload", sys->id, node->id());
    }

    std::vector<int> participants;
    if (comm_group != nullptr) {
        participants = comm_group->involved_NPUs;
    } else {
        for (const auto* participant : Sys::all_sys) {
            if (participant != nullptr) {
                participants.push_back(participant->id);
            }
        }
    }
    std::sort(participants.begin(), participants.end());
    if (participants.empty() ||
        std::adjacent_find(participants.begin(), participants.end()) !=
            participants.end()) {
        workload_barrier_fatal("invalid_participants", sys->id, node->id());
    }
    if (!std::binary_search(participants.begin(), participants.end(), sys->id)) {
        workload_barrier_fatal("rank_not_in_group", sys->id, node->id());
    }

    const WorkloadBarrierKey key{node->id(), node->name(), participants};
    auto& arrivals = workload_barrier_arrivals[key];
    const auto duplicate = std::find_if(
        arrivals.begin(), arrivals.end(),
        [this](const GlobalPlanBarrierArrival& item) {
            return item.rank == sys->id;
        });
    if (duplicate != arrivals.end()) {
        workload_barrier_fatal("duplicate_rank", sys->id, node->id());
    }
    arrivals.push_back({this, node->id(), sys->id});
    if (arrivals.size() < participants.size()) {
        return;
    }
    if (arrivals.size() != participants.size()) {
        workload_barrier_fatal("too_many_arrivals", sys->id, node->id());
    }
    std::set<int> ranks;
    for (const auto& item : arrivals) {
        ranks.insert(item.rank);
    }
    if (!std::equal(ranks.begin(), ranks.end(), participants.begin(),
                    participants.end())) {
        workload_barrier_fatal("rank_set_mismatch", sys->id, node->id());
    }

    auto* release = new WorkloadBarrierRelease{key, arrivals};
    workload_barrier_arrivals.erase(key);
    release_workload_barrier(release);
}

void Workload::issue_global_plan_round_barrier(
    shared_ptr<Chakra::ETFeederNode> node) {
    if (!node->has_other_attr("plan_global_round")) {
        global_plan_barrier_fatal("missing_round", sys->id, -1);
    }
    const ChakraProtoMsg::AttributeProto& attr =
        node->get_other_attr("plan_global_round");
    if (!attr.has_int64_val() || attr.int64_val() < 0) {
        global_plan_barrier_fatal("invalid_round", sys->id, -1);
    }
    const int64_t round = attr.int64_val();
    const std::string expected_name =
        "ocs/global-round-" + std::to_string(round) + "/barrier";
    if (node->name() != expected_name) {
        global_plan_barrier_fatal("invalid_name", sys->id, round);
    }
    if (round != next_global_plan_round) {
        global_plan_barrier_fatal("round_not_active", sys->id, round);
    }
    if (node->comm_size() != 0) {
        global_plan_barrier_fatal("nonzero_payload", sys->id, round);
    }

    auto& arrivals = global_plan_barrier_arrivals[round];
    const auto duplicate = std::find_if(
        arrivals.begin(), arrivals.end(),
        [this](const GlobalPlanBarrierArrival& item) {
            return item.rank == sys->id;
        });
    if (duplicate != arrivals.end()) {
        global_plan_barrier_fatal("duplicate_rank", sys->id, round);
    }
    arrivals.push_back({this, node->id(), sys->id});

    if (arrivals.size() < Sys::all_sys.size()) {
        return;
    }
    if (arrivals.size() != Sys::all_sys.size()) {
        global_plan_barrier_fatal("too_many_arrivals", sys->id, round);
    }
    std::set<int> ranks;
    for (const auto& item : arrivals) {
        ranks.insert(item.rank);
    }
    if (ranks.size() != Sys::all_sys.size()) {
        global_plan_barrier_fatal("rank_set_mismatch", sys->id, round);
    }

    auto* release = new GlobalPlanBarrierRelease{
        round, round + 1, arrivals};
    global_plan_barrier_arrivals.erase(round);
    sys->comm_NI->sim_wait_for_plan_round(
        release->target_round,
        &release_global_plan_round_barrier,
        release);
}

void Workload::issue_plane_plan_configuration_barrier(
    shared_ptr<Chakra::ETFeederNode> node) {
    if (!node->has_other_attr("plan_plane") ||
        !node->has_other_attr("plan_configuration")) {
        plane_plan_barrier_fatal("missing_identity", sys->id, -1, -1);
    }
    const ChakraProtoMsg::AttributeProto& plane_attr =
        node->get_other_attr("plan_plane");
    const ChakraProtoMsg::AttributeProto& configuration_attr =
        node->get_other_attr("plan_configuration");
    if (!plane_attr.has_int64_val() || plane_attr.int64_val() < 0 ||
        !configuration_attr.has_int64_val() ||
        configuration_attr.int64_val() < 0) {
        plane_plan_barrier_fatal("invalid_identity", sys->id, -1, -1);
    }
    const int plane = static_cast<int>(plane_attr.int64_val());
    const int configuration =
        static_cast<int>(configuration_attr.int64_val());
    const std::string expected_name =
        "ocs/plane-" + std::to_string(plane) + "/configuration-" +
        std::to_string(configuration) + "/active";
    if (node->name() != expected_name) {
        plane_plan_barrier_fatal(
            "invalid_name", sys->id, plane, configuration);
    }
    if (configuration < next_plane_plan_configuration[plane]) {
        plane_plan_barrier_fatal(
            "stale_configuration", sys->id, plane, configuration);
    }
    if (node->comm_size() != 0) {
        plane_plan_barrier_fatal(
            "nonzero_payload", sys->id, plane, configuration);
    }

    const PlanePlanBarrierKey key{plane, configuration};
    auto& arrivals = plane_plan_barrier_arrivals[key];
    const auto duplicate = std::find_if(
        arrivals.begin(), arrivals.end(),
        [this](const GlobalPlanBarrierArrival& item) {
            return item.rank == sys->id;
        });
    if (duplicate != arrivals.end()) {
        plane_plan_barrier_fatal(
            "duplicate_rank", sys->id, plane, configuration);
    }
    arrivals.push_back({this, node->id(), sys->id});
    try_release_plane_plan_configuration(plane);
}

void Workload::skip_invalid(shared_ptr<Chakra::ETFeederNode> node) {
    et_feeder->freeChildrenNodes(node->id());
    et_feeder->removeNode(node->id());
}

void Workload::call(EventType event, CallData* data) {
    if (is_finished) {
        return;
    }

    if (event == EventType::CollectiveCommunicationFinished) {
        IntData* int_data = (IntData*)data;
        hw_resource->tics_gpu_comms += int_data->execution_time;
        uint64_t node_id = collective_comm_node_id_map[int_data->data];
        shared_ptr<Chakra::ETFeederNode> node = et_feeder->lookupNode(node_id);

        if (sys->trace_enabled) {
            LoggerFactory::get_logger("workload")
                ->debug("callback,sys->id={}, tick={}, node->id={}, "
                        "node->name={}, node->type={}",
                        sys->id, Sys::boostedTick(), node->id(), node->name(),
                        static_cast<uint64_t>(node->type()));
        }

        hw_resource->release(node);

        et_feeder->freeChildrenNodes(node_id);

        issue_dep_free_nodes();
      
        // The Dataset class provides statistics that should be used later to dump
        // more statistics in the workload layer
        delete collective_comm_wrapper_map[int_data->data];
        collective_comm_wrapper_map.erase(int_data->data);
        et_feeder->removeNode(node_id);

    } else {
        if (data == nullptr) {
            issue_dep_free_nodes();
        } else {
            WorkloadLayerHandlerData* wlhd = (WorkloadLayerHandlerData*)data;
            shared_ptr<Chakra::ETFeederNode> node =
                et_feeder->lookupNode(wlhd->node_id);

            if (sys->trace_enabled) {
                LoggerFactory::get_logger("workload")
                    ->debug("callback,sys->id={}, tick={}, node->id={}, "
                            "node->name={}, node->type={}",
                            sys->id, Sys::boostedTick(), node->id(),
                            node->name(), static_cast<uint64_t>(node->type()));
            }

            const bool barrier =
                node->type() == ChakraNodeType::COMM_COLL_NODE &&
                node->comm_type() == ChakraCollectiveCommType::BARRIER;
            const bool occupies_hardware =
                !barrier ||
                (is_plan_round_barrier(node) &&
                 !is_plan_plane_barrier(node));
            if (occupies_hardware) {
                hw_resource->release(node);
            }

            et_feeder->freeChildrenNodes(node->id());

            issue_dep_free_nodes();

            et_feeder->removeNode(wlhd->node_id);
            delete wlhd;
        }
    }

    if (!et_feeder->hasNodesToIssue() &&
        (hw_resource->num_in_flight_cpu_ops == 0) &&
        (hw_resource->num_in_flight_gpu_comp_ops == 0) &&
        (hw_resource->num_in_flight_gpu_comm_ops == 0)) {
        report();
        sys->comm_NI->sim_notify_finished();
        is_finished = true;
    }
}

void Workload::fire() {
    call(EventType::General, NULL);
}

void Workload::report() {
    Tick curr_tick = Sys::boostedTick();
    LoggerFactory::get_logger("workload")
        ->info("sys[{}] finished, {} cycles, exposed communication {} cycles.",
               sys->id, curr_tick, curr_tick - hw_resource->tics_gpu_ops);
}
