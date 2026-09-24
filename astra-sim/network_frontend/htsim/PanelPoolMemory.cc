#include "PanelPoolMemory.hh"

#include <fstream>
#include <iostream>
#include <json/json.hpp>

#include "HTSimNetworkApi.hh"
#include "HTSimSession.hh"
#include "astra-sim/workload/Workload.hh"
#include "astra-sim/system/WorkloadLayerHandlerData.hh"

using json = nlohmann::json;
using namespace AstraSim;

namespace HTSim {

namespace {
// Chakra tensor_loc values written by LLMServingSim's converter.
const std::map<std::string, uint32_t> kTensorLoc = {
    {"LOCAL", 1}, {"REMOTE", 2}, {"CXL", 3}, {"STORAGE", 4}};
// Pool transfer tags live above every collective stream id ASTRA hands out.
constexpr int kPoolTagBase = 0x40000000;
constexpr int kPoolTagRange = 0x0FFFFFFF;
}  // namespace

struct PanelPoolMemory::Transfer {
    PanelPoolMemory* owner;
    WorkloadLayerHandlerData* wlhd;
    int pool;
    bool is_load;
    uint64_t bytes;
};

PanelPoolMemory::PanelPoolMemory(const std::string& config_path,
                                 std::unique_ptr<AstraRemoteMemoryAPI> fallback)
    : fallback_(std::move(fallback)), next_tag_(kPoolTagBase) {
    std::ifstream in(config_path);
    if (!in.good()) {
        std::cerr << "PanelPoolMemory: cannot open " << config_path << std::endl;
        exit(2);
    }
    json j;
    in >> j;
    for (const auto& p : j.at("pools")) {
        Pool pool;
        pool.id = p.at("id").get<std::string>();
        pool.endpoint = p.at("endpoint").get<int>();
        pool.bank = p.at("bank").get<int>();
        pool.capacity_bytes = p.value("capacity_bytes", uint64_t(0));
        pools_.push_back(pool);
    }
    if (j.contains("tensor_loc_pool")) {
        for (auto it = j["tensor_loc_pool"].begin(); it != j["tensor_loc_pool"].end(); ++it) {
            auto loc = kTensorLoc.find(it.key());
            if (loc == kTensorLoc.end()) {
                std::cerr << "PanelPoolMemory: unknown tensor location " << it.key() << std::endl;
                exit(2);
            }
            const std::string pool_id = it.value().get<std::string>();
            int idx = -1;
            for (size_t k = 0; k < pools_.size(); ++k)
                if (pools_[k].id == pool_id) idx = (int)k;
            if (idx < 0) {
                std::cerr << "PanelPoolMemory: tensor_loc_pool names unknown pool " << pool_id << std::endl;
                exit(2);
            }
            loc_to_pool_[loc->second] = idx;
        }
    }
    std::cerr << "PanelPoolMemory: " << pools_.size() << " pool(s)";
    for (const auto& p : pools_)
        std::cerr << " " << p.id << "(P=" << p.endpoint << ",B=" << p.bank << ")";
    std::cerr << "; " << loc_to_pool_.size() << " tensor location(s) routed to pools" << std::endl;
}

void PanelPoolMemory::set_sys(int id, Sys* sys) {
    sys_[id] = sys;
    if (fallback_) fallback_->set_sys(id, sys);
}

int PanelPoolMemory::pool_for(uint32_t tensor_loc, uint32_t tensor_device) const {
    auto it = loc_to_pool_.find(tensor_loc);
    if (it == loc_to_pool_.end()) return -1;
    // A location mapped to a pool selects among the configured pools by the
    // node's tensor_device when that is a valid index, else the mapped pool.
    if (tensor_device < pools_.size() && loc_to_pool_.size() == 1 && pools_.size() > 1)
        return (int)tensor_device;
    return it->second;
}

void PanelPoolMemory::issue(uint64_t tensor_size, WorkloadLayerHandlerData* wlhd) {
    auto node = wlhd->workload->et_feeder->lookupNode(wlhd->node_id);
    // The feeder keeps tensor_loc / tensor_device (written by the frontend's
    // converter as uint32 attributes) among the generic attributes; its
    // tensor_loc() accessor is never assigned.
    uint32_t loc = node->has_other_attr("tensor_loc")
        ? node->get_other_attr("tensor_loc").uint32_val() : node->tensor_loc();
    uint32_t device = 0;
    if (node->has_other_attr("tensor_device"))
        device = node->get_other_attr("tensor_device").uint32_val();
    const int pool = pool_for(loc, device);
    if (pool < 0) {
        ++fallback_issues_;
        if (fallback_issues_ == 1)
            std::cerr << "PanelPoolMemory: memory node " << wlhd->node_id << " at tensor_loc "
                      << loc << " is not mapped to a pool; using the analytical remote memory"
                      << std::endl;
        if (!fallback_) {
            std::cerr << "PanelPoolMemory: memory node " << wlhd->node_id
                      << " at tensor_loc " << node->tensor_loc()
                      << " is not mapped to a pool and there is no fallback backend" << std::endl;
            exit(2);
        }
        fallback_->issue(tensor_size, wlhd);
        return;
    }
    const bool is_load = node->type() == ChakraProtoMsg::MEM_LOAD_NODE;
    const int rank = wlhd->sys_id;
    const int bank = pools_[pool].bank;
    const int src = is_load ? bank : rank;
    const int dst = is_load ? rank : bank;
    if (tensor_size == 0 || tensor_size > (uint64_t)INT32_MAX) {
        std::cerr << "PanelPoolMemory: transfer of " << tensor_size
                  << " bytes is outside the flow size range (1 .. 2^31-1)" << std::endl;
        exit(2);
    }
    if (next_tag_ - kPoolTagBase >= kPoolTagRange) next_tag_ = kPoolTagBase;
    const int tag = next_tag_++;
    const int flow_id = (int)HTSimNetworkApi::next_flow_id();
    auto* t = new Transfer{this, wlhd, pool, is_load, tensor_size};
    Pool& p = pools_[pool];
    ++p.in_flight;
    if (is_load) { ++p.loads; p.load_bytes += tensor_size; }
    else { ++p.stores; p.store_bytes += tensor_size; }

    // The receive side is registered before the flow exists so the arrival
    // of the last byte finds a waiter: for a load that is the node's
    // completion; for a store it only consumes the delivery record.
    MsgEventKey key = std::make_pair(tag, std::make_pair(src, dst));
    HTSimSession::recv_waiting[key] = MsgEvent(src, dst, Dir::Receive, (int)tensor_size,
                                               is_load ? (void*)t : nullptr,
                                               is_load ? &PanelPoolMemory::transfer_done
                                                       : &PanelPoolMemory::ignore);
    const std::string uid = std::string("pool:") + p.id + ":" + (is_load ? "load:" : "store:")
        + std::to_string(rank) + ":" + std::to_string(wlhd->node_id);
    FlowInfo flow(src, dst, (int)tensor_size, tag, uid, tag);
    HTSimSession::instance().send_flow(flow, flow_id,
                                       is_load ? &PanelPoolMemory::ignore
                                               : &PanelPoolMemory::transfer_done,
                                       is_load ? nullptr : (void*)t);
}

void PanelPoolMemory::transfer_done(void* arg) {
    auto* t = static_cast<Transfer*>(arg);
    PanelPoolMemory* self = t->owner;
    --self->pools_[t->pool].in_flight;
    // Complete the memory node on the issuing rank through its own event
    // queue, exactly as the analytical backend does at the end of its delay.
    Sys* sys = self->sys_.at(t->wlhd->sys_id);
    sys->register_event(t->wlhd->workload, EventType::General, t->wlhd, 0);
    delete t;
}

void PanelPoolMemory::ignore(void*) {}

void PanelPoolMemory::report(std::ostream& os) const {
    for (const auto& p : pools_) {
        os << "POOL_STATS pool=" << p.id << " endpoint=" << p.endpoint << " bank=" << p.bank
           << " loads=" << p.loads << " load_bytes=" << p.load_bytes
           << " stores=" << p.stores << " store_bytes=" << p.store_bytes
           << " in_flight=" << p.in_flight << std::endl;
    }
    os << "POOL_STATS fallback_issues=" << fallback_issues_ << std::endl;
}

}  // namespace HTSim
