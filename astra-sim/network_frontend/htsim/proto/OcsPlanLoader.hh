// Plain-data OCS plan loader. Kept free of htsim headers: csg-htsim macros
// break nlohmann/json template metaprogramming when included in the same TU.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

struct OcsPlanData {
    int version = 0;
    int planes = 0;
    double reconfiguration_ns = 0.0;
    bool initial_reconfiguration = false;
    uint64_t scheduled_bytes = 0;
    int rounds = 0;
    // Per configuration in file order: exact transmitted stripes plus the
    // installed matching and a forced-reconfiguration marker.
    struct Stripe { std::string stripe_uid; int plane; uint64_t bytes; };
    struct Circuit { int src; int dst; uint64_t bytes;
                     std::string flow_uid; std::string stripe_uid; };
    struct Cfg { int plane; int stream; int round;
                 std::vector<Circuit> circuits;
                 std::vector<std::pair<int,int>> matching;
                 bool force_reconf = false;
                  bool synchronize = false;
                  // 0 = unspecified, 1 = fold (into owners), 2 = unfold
                  int phase = 0; };
    std::vector<Cfg> configurations;
    // legacy view: (src, dst, bytes, is_direct) in file order
    std::vector<std::tuple<int,int,uint64_t,bool>> assignments;
    // Complete plan-v6 logical-flow and per-stripe identities.
    struct Asn { std::string flow_uid; int src; int dst; uint64_t bytes;
                 int tag; int stream; int round; bool is_direct;
                 std::vector<Stripe> stripes; int phase = 0; };
    std::vector<Asn> assignments_full;
};

// Exact identity lookup shared by qualification tests and the HTSim runtime.
// Slots are (plane, per-plane configuration index), matching HTSimProtoTcp.
struct OcsPlanIdentityIndex {
    std::map<std::string, OcsPlanData::Asn> assignments;
    std::map<std::string, std::pair<int, int>> stripe_slots;
};

bool load_ocs_plan_file(const std::string& path, OcsPlanData& out,
                        std::string& error);
bool build_ocs_plan_identity_index(const OcsPlanData& plan,
                                   OcsPlanIdentityIndex& out,
                                   std::string& error);
bool consume_ocs_assignment(OcsPlanIdentityIndex& index,
                            const std::string& flow_uid,
                            OcsPlanData::Asn& out);
bool consume_ocs_stripe_slot(OcsPlanIdentityIndex& index,
                             const std::string& stripe_uid,
                             std::pair<int, int>& out);
