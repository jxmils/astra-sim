// Plain-data OCS plan loader. Kept free of htsim headers: csg-htsim macros
// break nlohmann/json template metaprogramming when included in the same TU.
#pragma once
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

struct OcsPlanData {
    int planes = 0;
    double reconfiguration_ns = 0.0;
    bool initial_reconfiguration = false;
    uint64_t scheduled_bytes = 0;
    int rounds = 0;
    // per configuration in file order: (plane, stream, circuits[(src,dst,bytes)])
    struct Cfg { int plane; int stream; int round;
                 std::vector<std::tuple<int,int,uint64_t>> circuits; };
    std::vector<Cfg> configurations;
    // assignments in file order: (src, dst, bytes, is_direct)
    std::vector<std::tuple<int,int,uint64_t,bool>> assignments;
};

bool load_ocs_plan_file(const std::string& path, OcsPlanData& out,
                        std::string& error);
