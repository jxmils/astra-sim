#include "OcsPlanLoader.hh"
#include <json/json.hpp>
#include <fstream>

bool load_ocs_plan_file(const std::string& path, OcsPlanData& out,
                        std::string& error) {
    std::ifstream f(path);
    if (!f) { error = "cannot open " + path; return false; }
    nlohmann::json p;
    try { f >> p; } catch (const std::exception& e) { error = e.what(); return false; }
    out.planes = p["planes"].get<int>();
    out.reconfiguration_ns = p["reconfiguration_ns"].get<double>();
    out.initial_reconfiguration = p.value("initial_reconfiguration", false);
    int ridx = 0;
    for (auto& round : p["rounds"]) {
        for (auto& cfg : round["configurations"]) {
            OcsPlanData::Cfg oc;
            oc.plane = cfg["plane"].get<int>();
            oc.stream = cfg.value("stream", -1);
            oc.round = ridx;
            for (auto& ci : cfg["circuits"]) {
                uint64_t b = ci["bytes"].get<uint64_t>();
                oc.circuits.push_back(std::make_tuple(
                    ci["source"].get<int>(), ci["destination"].get<int>(), b));
                out.scheduled_bytes += b;
            }
            out.configurations.push_back(oc);
        }
        ridx++;
    }
    out.rounds = ridx;
    for (auto& a : p["assignments"]) {
        out.assignments.push_back(std::make_tuple(
            a["source"].get<int>(), a["destination"].get<int>(),
            a["bytes"].get<uint64_t>(),
            a["route"].get<std::string>() == "DIRECT"));
    }
    return true;
}
