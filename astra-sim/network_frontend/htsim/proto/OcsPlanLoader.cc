#include "OcsPlanLoader.hh"
#include <json/json.hpp>
#include <cctype>
#include <fstream>
#include <map>
#include <set>

static int phase_of(const nlohmann::json& j) {
    if (!j.contains("phase")) return 0;
    std::string s = j["phase"].get<std::string>();
    return s == "fold" ? 1 : (s == "unfold" ? 2 : 0);
}

static bool is_optical_route(const std::string& route) {
    if (route == "OCS") return true;
    if (route.size() <= 3 || route.substr(0, 3) != "OCS") return false;
    for (size_t index = 3; index < route.size(); index++)
        if (!std::isdigit(static_cast<unsigned char>(route[index]))) return false;
    return true;
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
        out.endpoints = p.at("endpoints").get<int>();
        out.planes = p.at("planes").get<int>();
        out.reconfiguration_ns = p.at("reconfiguration_ns").get<double>();
        if (out.endpoints < 2 || out.planes < 1 || out.reconfiguration_ns < 0) {
            error = "invalid plan-v6 topology or reconfiguration latency";
            return false;
        }
        out.initial_reconfiguration = p.value("initial_reconfiguration", false);

        std::map<std::string, OcsPlanData::Circuit> circuits_by_uid;
        std::map<std::string, std::pair<int, int>> circuit_locations;
        int ridx = 0;
        for (const auto& round : p.at("rounds")) {
            if (round.at("index").get<int>() != ridx) {
                error = "plan-v6 round indices are not contiguous"; return false;
            }
            const bool synchronize = round.value("synchronize", false);
            std::set<int> round_planes;
            for (const auto& cfg : round.at("configurations")) {
                OcsPlanData::Cfg oc;
                oc.plane = cfg.at("plane").get<int>();
                if (oc.plane < 0 || oc.plane >= out.planes ||
                    !round_planes.insert(oc.plane).second) {
                    error = "plan-v6 configuration plane is invalid or duplicated";
                    return false;
                }
                oc.stream = cfg.at("stream").get<int>();
                oc.round = ridx;
                oc.force_reconf = cfg.value("force_reconfiguration", false);
                oc.synchronize = synchronize;
                oc.phase = phase_of(cfg);

                std::set<int> matching_sources, matching_destinations;
                std::set<std::pair<int, int>> matching_edges;
                for (const auto& item : cfg.at("matching")) {
                    if (!item.is_array() || item.size() != 2) {
                        error = "plan-v6 matching entry is malformed"; return false;
                    }
                    const int source = item[0].get<int>();
                    const int destination = item[1].get<int>();
                    if (source < 0 || destination < 0 ||
                        source >= out.endpoints || destination >= out.endpoints ||
                        source == destination ||
                        !matching_sources.insert(source).second ||
                        !matching_destinations.insert(destination).second ||
                        !matching_edges.insert({source, destination}).second) {
                        error = "plan-v6 matching is not a valid directed matching";
                        return false;
                    }
                    oc.matching.push_back({source, destination});
                }

                for (const auto& ci : cfg.at("circuits")) {
                    OcsPlanData::Circuit circuit;
                    circuit.src = ci.at("source").get<int>();
                    circuit.dst = ci.at("destination").get<int>();
                    circuit.bytes = ci.at("bytes").get<uint64_t>();
                    circuit.flow_uid = ci.at("flow_uid").get<std::string>();
                    circuit.stripe_uid = ci.at("stripe_uid").get<std::string>();
                    if (circuit.src < 0 || circuit.dst < 0 ||
                        circuit.src >= out.endpoints || circuit.dst >= out.endpoints ||
                        circuit.src == circuit.dst || circuit.bytes == 0 ||
                        circuit.flow_uid.empty() || circuit.stripe_uid.empty()) {
                        error = "plan-v6 circuit fields are invalid"; return false;
                    }
                    if (!matching_edges.count({circuit.src, circuit.dst})) {
                        error = "plan-v6 circuit is outside its installed matching";
                        return false;
                    }
                    if (!circuits_by_uid.emplace(
                            circuit.stripe_uid, circuit).second) {
                        error = "duplicate plan-v6 stripe_uid " + circuit.stripe_uid;
                        return false;
                    }
                    circuit_locations[circuit.stripe_uid] = {ridx, oc.plane};
                    oc.circuits.push_back(circuit);
                    out.scheduled_bytes += circuit.bytes;
                }
                out.configurations.push_back(oc);
            }
            ridx++;
        }
        out.rounds = ridx;

        std::set<std::string> flow_uids;
        std::set<std::string> assigned_stripe_uids;
        for (const auto& a : p.at("assignments")) {
            const std::string route = a.at("route").get<std::string>();
            if (route != "DIRECT" && !is_optical_route(route)) {
                error = "plan-v6 assignment route is invalid"; return false;
            }
            const bool direct = route == "DIRECT";
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
            an.src = a.at("source").get<int>();
            an.dst = a.at("destination").get<int>();
            an.bytes = a.at("logical_bytes").get<uint64_t>();
            an.tag = a.at("tag").get<int>();
            an.stream = a.at("stream").get<int>();
            an.round = a.at("round").get<int>();
            an.is_direct = direct;
            an.phase = phase_of(a);
            if (an.src < 0 || an.dst < 0 || an.src >= out.endpoints ||
                an.dst >= out.endpoints || an.src == an.dst || an.bytes == 0 ||
                an.tag < 0 || an.stream < 0) {
                error = "plan-v6 assignment fields are invalid"; return false;
            }

            uint64_t stripe_bytes = 0;
            if (!direct) {
                if (an.round < 0 || an.round >= out.rounds ||
                    !a.contains("stripes") || a.at("stripes").empty()) {
                    error = "plan-v6 optical assignment has no valid round/stripes";
                    return false;
                }
                for (const auto& s : a.at("stripes")) {
                    OcsPlanData::Stripe stripe;
                    stripe.stripe_uid = s.at("stripe_uid").get<std::string>();
                    stripe.plane = s.at("plane").get<int>();
                    stripe.bytes = s.at("bytes").get<uint64_t>();
                    if (stripe.stripe_uid.empty() || stripe.bytes == 0 ||
                        stripe.plane < 0 || stripe.plane >= out.planes ||
                        !assigned_stripe_uids.insert(stripe.stripe_uid).second) {
                        error = "assignment stripe_uid/plane/bytes is invalid or duplicated";
                        return false;
                    }
                    if (route.size() > 3 &&
                        std::stoi(route.substr(3)) != stripe.plane) {
                        error = "assignment route names the wrong plane"; return false;
                    }
                    stripe_bytes += stripe.bytes;
                    const auto circuit = circuits_by_uid.find(stripe.stripe_uid);
                    const auto location = circuit_locations.find(stripe.stripe_uid);
                    if (circuit == circuits_by_uid.end() ||
                        location == circuit_locations.end() ||
                        circuit->second.flow_uid != an.flow_uid ||
                        circuit->second.src != an.src ||
                        circuit->second.dst != an.dst ||
                        circuit->second.bytes != stripe.bytes ||
                        location->second.first != an.round ||
                        location->second.second != stripe.plane) {
                        error = "plan-v6 stripe/circuit identity mismatch"; return false;
                    }
                    an.stripes.push_back(stripe);
                }
                if (stripe_bytes != an.bytes) {
                    error = "plan-v6 stripes do not reconstruct logical_bytes";
                    return false;
                }
            } else {
                if (an.round != -1 ||
                    (a.contains("stripes") && !a.at("stripes").empty())) {
                    error = "plan-v6 direct assignment contains an optical slot";
                    return false;
                }
            }
            out.assignments_full.push_back(an);
        }
        if (assigned_stripe_uids.size() != circuits_by_uid.size()) {
            error = "plan-v6 contains an unreferenced circuit stripe"; return false;
        }
        return true;
    } catch (const std::exception& exception) {
        error = std::string("malformed plan-v6: ") + exception.what();
        return false;
    }
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

OcsSlotState classify_ocs_slot_state(int configuration, int current,
                                     bool dark) {
    if (configuration < current) return OcsSlotState::Stale;
    if (configuration > current) return OcsSlotState::Future;
    if (dark) return OcsSlotState::Dark;
    return OcsSlotState::Active;
}
