// Plain-data OCS plan loader. Kept free of htsim headers: csg-htsim macros
// break nlohmann/json template metaprogramming when included in the same TU.
#pragma once
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

struct OcsPlanData {
    int planes = 0;
    double reconfiguration_ns = 0.0;
    bool initial_reconfiguration = false;
    uint64_t scheduled_bytes = 0;
    int rounds = 0;
    // per configuration in file order: transmitted circuits plus the installed
    // matching (v5 may install a superset of what transmits) and a forced-
    // reconfiguration marker.
    struct Cfg { int plane; int stream; int round;
                 std::vector<std::tuple<int,int,uint64_t>> circuits;
                 std::vector<std::pair<int,int>> matching;
                 bool force_reconf = false;
                  bool synchronize = false;
                  // 0 = unspecified, 1 = fold (into owners), 2 = unfold
                  int phase = 0; };
    std::vector<Cfg> configurations;
    // legacy view: (src, dst, bytes, is_direct) in file order
    std::vector<std::tuple<int,int,uint64_t,bool>> assignments;
    // full v5 records: stream identity and per-plane byte stripes.
    struct Asn { int src; int dst; uint64_t bytes; int stream; bool is_direct;
                 std::vector<std::pair<int,uint64_t>> stripes;
                 int phase = 0; };
    std::vector<Asn> assignments_full;
};

bool load_ocs_plan_file(const std::string& path, OcsPlanData& out,
                        std::string& error);
