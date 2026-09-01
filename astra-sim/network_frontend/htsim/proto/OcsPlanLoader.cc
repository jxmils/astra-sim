#include "OcsPlanLoader.hh"
#include <json/json.hpp>
#include <fstream>

static int phase_of(const nlohmann::json& j) {
    if (!j.contains("phase")) return 0;
    std::string s = j["phase"].get<std::string>();
    return s == "fold" ? 1 : (s == "unfold" ? 2 : 0);
}

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
        bool synchronize = round.value("synchronize", false);
        for (auto& cfg : round["configurations"]) {
            OcsPlanData::Cfg oc;
            oc.plane = cfg["plane"].get<int>();
            oc.stream = cfg.value("stream", -1);
            oc.round = ridx;
            oc.force_reconf = cfg.value("force_reconfiguration", false);
            oc.synchronize = synchronize;
            oc.phase = phase_of(cfg);
            for (auto& ci : cfg["circuits"]) {
                uint64_t b = ci["bytes"].get<uint64_t>();
                oc.circuits.push_back(std::make_tuple(
                    ci["source"].get<int>(), ci["destination"].get<int>(), b));
                out.scheduled_bytes += b;
            }
            if (cfg.contains("matching")) {
                for (auto& m : cfg["matching"])
                    oc.matching.push_back({m[0].get<int>(), m[1].get<int>()});
            }
            out.configurations.push_back(oc);
        }
        ridx++;
    }
    out.rounds = ridx;
    for (auto& a : p["assignments"]) {
        bool direct = a["route"].get<std::string>() == "DIRECT";
        out.assignments.push_back(std::make_tuple(
            a["source"].get<int>(), a["destination"].get<int>(),
            a["bytes"].get<uint64_t>(), direct));
        if (a.value("not_before_ns", 0) != 0) {
            error = "plan uses not_before_ns (unsupported)"; return false;
        }
        if (a.value("allow_direct_escape", false)) {
            error = "plan uses direct escape (unsupported)"; return false;
        }
        OcsPlanData::Asn an;
        an.src = a["source"].get<int>();
        an.dst = a["destination"].get<int>();
        an.bytes = a["bytes"].get<uint64_t>();
        an.stream = a.value("stream", -1);
        an.is_direct = direct;
        an.phase = phase_of(a);
        if (!direct && a.contains("stripes")) {
            for (auto& s : a["stripes"])
                an.stripes.push_back({s["plane"].get<int>(),
                                      s["bytes"].get<uint64_t>()});
        }
        out.assignments_full.push_back(an);
    }
    return true;
}
