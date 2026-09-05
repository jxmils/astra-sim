#include "OcsPlanLoader.hh"
#include <json/json.hpp>
#include <fstream>
#include <map>
#include <set>

static int phase_of(const nlohmann::json& j) {
    if (!j.contains("phase")) return 0;
    std::string s = j["phase"].get<std::string>();
    return s == "fold" ? 1 : (s == "unfold" ? 2 : 0);
}

bool load_ocs_plan_file(const std::string& path, OcsPlanData& out,
                        std::string& error) {
    out = OcsPlanData();
    std::ifstream f(path);
    if (!f) { error = "cannot open " + path; return false; }
    nlohmann::json p;
    try { f >> p; } catch (const std::exception& e) { error = e.what(); return false; }
    if (p.value("format", "") != "panel-ocs-plan") {
        error = "unsupported OCS plan format"; return false;
    }
    out.version = p.value("version", 0);
    if (out.version != 6) {
        error = "backend-v2 requires panel-ocs-plan version 6"; return false;
    }
    try {
        out.planes = p.at("planes").get<int>();
        out.reconfiguration_ns = p.at("reconfiguration_ns").get<double>();
    } catch (const std::exception& e) {
        error = e.what(); return false;
    }
    out.initial_reconfiguration = p.value("initial_reconfiguration", false);
    std::map<std::string, OcsPlanData::Circuit> circuits_by_uid;
    std::map<std::string, std::pair<int, int>> circuit_locations;
    int ridx = 0;
    for (auto& round : p.at("rounds")) {
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
                OcsPlanData::Circuit circuit;
                circuit.src = ci.at("source").get<int>();
                circuit.dst = ci.at("destination").get<int>();
                circuit.bytes = b;
                circuit.flow_uid = ci.at("flow_uid").get<std::string>();
                circuit.stripe_uid = ci.at("stripe_uid").get<std::string>();
                if (circuit.flow_uid.empty() || circuit.stripe_uid.empty()) {
                    error = "plan-v6 circuit identity must be nonempty"; return false;
                }
                if (!circuits_by_uid.emplace(
                        circuit.stripe_uid, circuit).second) {
                    error = "duplicate plan-v6 stripe_uid " + circuit.stripe_uid;
                    return false;
                }
                circuit_locations[circuit.stripe_uid] = {ridx, oc.plane};
                oc.circuits.push_back(circuit);
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
    std::set<std::string> flow_uids;
    std::set<std::string> assigned_stripe_uids;
    for (auto& a : p.at("assignments")) {
        bool direct = a["route"].get<std::string>() == "DIRECT";
        uint64_t logical_bytes = a.at("logical_bytes").get<uint64_t>();
        out.assignments.push_back(std::make_tuple(
            a["source"].get<int>(), a["destination"].get<int>(),
            logical_bytes, direct));
        if (a.value("not_before_ns", 0) != 0) {
            error = "plan uses not_before_ns (unsupported)"; return false;
        }
        if (a.value("allow_direct_escape", false)) {
            error = "plan uses direct escape (unsupported)"; return false;
        }
        OcsPlanData::Asn an;
        an.flow_uid = a.at("flow_uid").get<std::string>();
        if (an.flow_uid.empty() || !flow_uids.insert(an.flow_uid).second) {
            error = "plan-v6 flow_uid is empty or duplicated"; return false;
        }
        an.src = a["source"].get<int>();
        an.dst = a["destination"].get<int>();
        an.bytes = logical_bytes;
        an.tag = a.at("tag").get<int>();
        an.stream = a.value("stream", -1);
        an.round = a.at("round").get<int>();
        an.is_direct = direct;
        an.phase = phase_of(a);
        uint64_t stripe_bytes = 0;
        if (!direct) {
            if (!a.contains("stripes") || a["stripes"].empty()) {
                error = "plan-v6 optical assignment has no stripes"; return false;
            }
            for (auto& s : a["stripes"]) {
                OcsPlanData::Stripe stripe;
                stripe.stripe_uid = s.at("stripe_uid").get<std::string>();
                stripe.plane = s.at("plane").get<int>();
                stripe.bytes = s.at("bytes").get<uint64_t>();
                stripe_bytes += stripe.bytes;
                if (stripe.stripe_uid.empty() ||
                    !assigned_stripe_uids.insert(stripe.stripe_uid).second) {
                    error = "assignment stripe_uid is empty or duplicated";
                    return false;
                }
                auto circuit = circuits_by_uid.find(stripe.stripe_uid);
                auto location = circuit_locations.find(stripe.stripe_uid);
                if (circuit == circuits_by_uid.end() ||
                    location == circuit_locations.end() ||
                    circuit->second.flow_uid != an.flow_uid ||
                    circuit->second.src != an.src ||
                    circuit->second.dst != an.dst ||
                    circuit->second.bytes != stripe.bytes ||
                    circuit->second.stripe_uid != stripe.stripe_uid ||
                    location->second.first != an.round ||
                    location->second.second != stripe.plane ||
                    stripe.plane < 0 || stripe.plane >= out.planes) {
                    error = "plan-v6 stripe/circuit identity mismatch"; return false;
                }
                an.stripes.push_back(stripe);
            }
            if (stripe_bytes != an.bytes) {
                error = "plan-v6 stripes do not reconstruct logical_bytes";
                return false;
            }
        } else if (a.contains("stripes") && !a["stripes"].empty()) {
            error = "plan-v6 direct assignment contains optical stripes";
            return false;
        }
        out.assignments_full.push_back(an);
    }
    if (assigned_stripe_uids.size() != circuits_by_uid.size()) {
        error = "plan-v6 contains an unreferenced circuit stripe"; return false;
    }
    return true;
}

bool build_ocs_plan_identity_index(const OcsPlanData& plan,
                                   OcsPlanIdentityIndex& out,
                                   std::string& error) {
    out = OcsPlanIdentityIndex();
    for (const auto& assignment : plan.assignments_full) {
        if (!out.assignments.emplace(
                assignment.flow_uid, assignment).second) {
            error = "duplicate flow_uid while indexing plan-v6";
            return false;
        }
    }

    std::vector<int> per_plane_index(plan.planes, 0);
    for (const auto& configuration : plan.configurations) {
        if (configuration.plane < 0 || configuration.plane >= plan.planes) {
            error = "configuration plane is outside plan while indexing";
            return false;
        }
        const int slot = per_plane_index[configuration.plane]++;
        for (const auto& circuit : configuration.circuits) {
            if (!out.stripe_slots.emplace(
                    circuit.stripe_uid,
                    std::make_pair(configuration.plane, slot)).second) {
                error = "duplicate stripe_uid while indexing plan-v6";
                return false;
            }
        }
    }
    return true;
}

bool consume_ocs_assignment(OcsPlanIdentityIndex& index,
                            const std::string& flow_uid,
                            OcsPlanData::Asn& out) {
    const auto item = index.assignments.find(flow_uid);
    if (item == index.assignments.end()) return false;
    out = item->second;
    index.assignments.erase(item);
    return true;
}

bool consume_ocs_stripe_slot(OcsPlanIdentityIndex& index,
                             const std::string& stripe_uid,
                             std::pair<int, int>& out) {
    const auto item = index.stripe_slots.find(stripe_uid);
    if (item == index.stripe_slots.end()) return false;
    out = item->second;
    index.stripe_slots.erase(item);
    return true;
}
