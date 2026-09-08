#include "OcsPlanLoader.hh"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {
bool expect(bool condition, const std::string& message) {
    if (!condition) std::cerr << "FAIL: " << message << std::endl;
    return condition;
}

std::string write_fixture(const std::string& body, const std::string& suffix) {
    const std::string path = "/tmp/ocs-plan-identity-" +
        std::to_string(static_cast<long long>(getpid())) + suffix + ".json";
    std::ofstream output(path);
    output << body;
    return path;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 2) {
        OcsPlanData loaded;
        std::string error;
        if (!load_ocs_plan_file(argv[1], loaded, error)) {
            std::cerr << "FAIL: generated plan does not load: " << error
                      << std::endl;
            return 1;
        }
        OcsPlanIdentityIndex index;
        if (!build_ocs_plan_identity_index(loaded, index, error)) {
            std::cerr << "FAIL: generated plan does not index: " << error
                      << std::endl;
            return 1;
        }
        for (const auto& expected : loaded.assignments_full) {
            OcsPlanData::Asn observed;
            if (!consume_ocs_assignment(index, expected.flow_uid, observed) ||
                observed.flow_uid != expected.flow_uid ||
                observed.stripes.size() != expected.stripes.size()) {
                std::cerr << "FAIL: flow identity did not round-trip: "
                          << expected.flow_uid << std::endl;
                return 1;
            }
            for (const auto& stripe : expected.stripes) {
                std::pair<int, int> slot(-1, -1);
                if (!consume_ocs_stripe_slot(
                        index, stripe.stripe_uid, slot) ||
                    slot.first != stripe.plane) {
                    std::cerr << "FAIL: stripe identity did not round-trip: "
                              << stripe.stripe_uid << std::endl;
                    return 1;
                }
            }
        }
        if (!index.assignments.empty() || !index.stripe_slots.empty()) {
            std::cerr << "FAIL: generated plan left unconsumed identities"
                      << std::endl;
            return 1;
        }
        std::cout << "PASS: generated plan loader round-trip" << std::endl;
        return 0;
    }
    if (argc != 1) {
        std::cerr << "usage: OcsPlanIdentityTest [plan-v6.json]" << std::endl;
        return 2;
    }

    const std::string plan = R"JSON({
  "format": "panel-ocs-plan",
  "version": 6,
  "endpoints": 6,
  "planes": 6,
  "reconfiguration_ns": 10.0,
  "rounds": [
    {"index": 0, "synchronize": false, "configurations": [
      {"plane": 0, "stream": 7, "matching": [[0,1],[2,3]], "circuits": [
        {"source":0,"destination":1,"bytes":1048576,"flow_uid":"A","stripe_uid":"A.0"},
        {"source":2,"destination":3,"bytes":100,"flow_uid":"S6","stripe_uid":"S6.0"}]},
      {"plane": 1, "stream": 7, "matching": [[0,1],[2,3]], "circuits": [
        {"source":0,"destination":1,"bytes":1048576,"flow_uid":"B","stripe_uid":"B.1"},
        {"source":2,"destination":3,"bytes":100,"flow_uid":"S6","stripe_uid":"S6.1"}]},
      {"plane": 2, "stream": 7, "matching": [[2,3]], "circuits": [
        {"source":2,"destination":3,"bytes":100,"flow_uid":"S6","stripe_uid":"S6.2"}]},
      {"plane": 3, "stream": 7, "matching": [[2,3]], "circuits": [
        {"source":2,"destination":3,"bytes":100,"flow_uid":"S6","stripe_uid":"S6.3"}]},
      {"plane": 4, "stream": 7, "matching": [[2,3]], "circuits": [
        {"source":2,"destination":3,"bytes":100,"flow_uid":"S6","stripe_uid":"S6.4"}]},
      {"plane": 5, "stream": 7, "matching": [[2,3]], "circuits": [
        {"source":2,"destination":3,"bytes":100,"flow_uid":"S6","stripe_uid":"S6.5"}]}
    ]},
    {"index": 1, "synchronize": false, "configurations": [
      {"plane": 0, "stream": 7, "matching": [[0,1],[4,5]], "circuits": [
        {"source":0,"destination":1,"bytes":1048576,"flow_uid":"C","stripe_uid":"C.0"},
        {"source":4,"destination":5,"bytes":512,"flow_uid":"S2","stripe_uid":"S2.0"}]},
      {"plane": 1, "stream": 7, "matching": [[4,5]], "circuits": [
        {"source":4,"destination":5,"bytes":512,"flow_uid":"S2","stripe_uid":"S2.1"}]}
    ]}
  ],
  "assignments": [
    {"flow_uid":"A","source":0,"destination":1,"tag":7,"stream":7,"logical_bytes":1048576,"round":0,"route":"OCS","stripes":[{"stripe_uid":"A.0","plane":0,"bytes":1048576}]},
    {"flow_uid":"S6","source":2,"destination":3,"tag":7,"stream":7,"logical_bytes":600,"round":0,"route":"OCS","stripes":[
      {"stripe_uid":"S6.0","plane":0,"bytes":100},{"stripe_uid":"S6.1","plane":1,"bytes":100},
      {"stripe_uid":"S6.2","plane":2,"bytes":100},{"stripe_uid":"S6.3","plane":3,"bytes":100},
      {"stripe_uid":"S6.4","plane":4,"bytes":100},{"stripe_uid":"S6.5","plane":5,"bytes":100}]},
    {"flow_uid":"C","source":0,"destination":1,"tag":7,"stream":7,"logical_bytes":1048576,"round":1,"route":"OCS","stripes":[{"stripe_uid":"C.0","plane":0,"bytes":1048576}]},
    {"flow_uid":"S2","source":4,"destination":5,"tag":7,"stream":7,"logical_bytes":1024,"round":1,"route":"OCS","stripes":[{"stripe_uid":"S2.0","plane":0,"bytes":512},{"stripe_uid":"S2.1","plane":1,"bytes":512}]},
    {"flow_uid":"B","source":0,"destination":1,"tag":7,"stream":7,"logical_bytes":1048576,"round":0,"route":"OCS","stripes":[{"stripe_uid":"B.1","plane":1,"bytes":1048576}]}
  ]
})JSON";

    bool ok = true;
    const std::string path = write_fixture(plan, "-v6");
    OcsPlanData loaded;
    std::string error;
    ok &= expect(load_ocs_plan_file(path, loaded, error),
                 "plan-v6 loads: " + error);
    ok &= expect(loaded.version == 6, "schema version is preserved");
    ok &= expect(loaded.assignments_full.size() == 5,
                 "every logical flow remains independent");

    OcsPlanIdentityIndex index;
    error.clear();
    ok &= expect(build_ocs_plan_identity_index(loaded, index, error),
                 "identity index builds: " + error);

    // B is consumed first although B is last in plan order and is tuple-identical
    // to A. Exact identities must select different physical planes.
    OcsPlanData::Asn assignment;
    std::pair<int, int> slot(-1, -1);
    ok &= expect(consume_ocs_assignment(index, "B", assignment),
                 "B is found by exact flow_uid");
    ok &= expect(assignment.stripes.size() == 1 &&
                 assignment.stripes[0].stripe_uid == "B.1",
                 "B preserves its stripe identity");
    ok &= expect(consume_ocs_stripe_slot(index, "B.1", slot) && slot.first == 1,
                 "B consumes plane 1");
    ok &= expect(consume_ocs_assignment(index, "A", assignment),
                 "A is found after B");
    ok &= expect(consume_ocs_stripe_slot(index, "A.0", slot) && slot.first == 0,
                 "A consumes plane 0");

    ok &= expect(consume_ocs_assignment(index, "S2", assignment) &&
                 assignment.stripes.size() == 2,
                 "two-plane flow preserves both stripes");
    for (int plane = 0; plane < 2; ++plane) {
        const std::string uid = "S2." + std::to_string(plane);
        ok &= expect(consume_ocs_stripe_slot(index, uid, slot) &&
                     slot.first == plane && slot.second == 1,
                     "two-plane stripe maps to exact round slot");
    }

    ok &= expect(consume_ocs_assignment(index, "S6", assignment) &&
                 assignment.stripes.size() == 6,
                 "six-plane flow preserves all stripes");
    for (int plane = 0; plane < 6; ++plane) {
        const std::string uid = "S6." + std::to_string(plane);
        ok &= expect(consume_ocs_stripe_slot(index, uid, slot) &&
                     slot.first == plane && slot.second == 0,
                     "six-plane stripe maps to exact plane");
    }

    ok &= expect(consume_ocs_assignment(index, "C", assignment),
                 "repeated matching flow remains distinct");
    ok &= expect(consume_ocs_stripe_slot(index, "C.0", slot) &&
                 slot.first == 0 && slot.second == 1,
                 "repeated matching selects its later round slot");
    ok &= expect(!consume_ocs_assignment(index, "A", assignment),
                 "a flow identity cannot be consumed twice");

    ok &= expect(classify_ocs_slot_state(2, 2, false) == OcsSlotState::Active,
                 "active configuration is accepted");
    ok &= expect(classify_ocs_slot_state(1, 2, false) == OcsSlotState::Stale,
                 "past configuration is classified stale");
    ok &= expect(classify_ocs_slot_state(3, 2, false) == OcsSlotState::Future,
                 "future configuration is classified inactive");
    ok &= expect(classify_ocs_slot_state(2, 2, true) == OcsSlotState::Dark,
                 "current configuration is inactive while the plane is dark");

    const std::string plan_v7 = R"JSON({
  "format": "panel-ocs-plan",
  "version": 7,
  "execution_model": "independent_planes",
  "endpoints": 4,
  "planes": 2,
  "reconfiguration_ns": 1.0,
  "plane_sequences": [
    {"plane": 0, "configurations": [
      {"sequence": 0, "stream": 10, "matching": [[0,1]], "circuits": [
        {"source":0,"destination":1,"bytes":800,"flow_uid":"P0A","stripe_uid":"P0A.0"}]},
      {"sequence": 1, "stream": 11, "matching": [[0,2]], "circuits": [
        {"source":0,"destination":2,"bytes":200,"flow_uid":"P0B","stripe_uid":"P0B.0"}]}
    ]},
    {"plane": 1, "configurations": [
      {"sequence": 0, "stream": 12, "matching": [[2,3]], "circuits": [
        {"source":2,"destination":3,"bytes":500,"flow_uid":"P1A","stripe_uid":"P1A.0"}]},
      {"sequence": 1, "stream": 13, "matching": [[2,0]], "circuits": [
        {"source":2,"destination":0,"bytes":500,"flow_uid":"P1B","stripe_uid":"P1B.0"}]}
    ]}
  ],
  "assignments": [
    {"flow_uid":"P0A","source":0,"destination":1,"tag":10,"stream":10,"logical_bytes":800,"route":"OCS0","stripes":[{"stripe_uid":"P0A.0","plane":0,"configuration":0,"bytes":800}]},
    {"flow_uid":"P0B","source":0,"destination":2,"tag":11,"stream":11,"logical_bytes":200,"route":"OCS0","stripes":[{"stripe_uid":"P0B.0","plane":0,"configuration":1,"bytes":200}]},
    {"flow_uid":"P1A","source":2,"destination":3,"tag":12,"stream":12,"logical_bytes":500,"route":"OCS1","stripes":[{"stripe_uid":"P1A.0","plane":1,"configuration":0,"bytes":500}]},
    {"flow_uid":"P1B","source":2,"destination":0,"tag":13,"stream":13,"logical_bytes":500,"route":"OCS1","stripes":[{"stripe_uid":"P1B.0","plane":1,"configuration":1,"bytes":500}]}
  ]
})JSON";
    const std::string v7_path = write_fixture(plan_v7, "-v7");
    error.clear();
    ok &= expect(load_ocs_plan_file(v7_path, loaded, error),
                 "plan-v7 loads: " + error);
    ok &= expect(loaded.version == 7 &&
                 loaded.execution_model == "independent_planes",
                 "plan-v7 execution identity is explicit");
    ok &= expect(loaded.rounds == 0 && loaded.configurations.size() == 4,
                 "plan-v7 has plane-local configurations without rounds");
    error.clear();
    ok &= expect(build_ocs_plan_identity_index(loaded, index, error),
                 "plan-v7 identity index builds: " + error);
    ok &= expect(consume_ocs_assignment(index, "P1B", assignment),
                 "plan-v7 flow identity is exact");
    ok &= expect(consume_ocs_stripe_slot(index, "P1B.0", slot) &&
                 slot.first == 1 && slot.second == 1,
                 "plan-v7 stripe selects an exact plane-local sequence");

    std::string rounded_v7 = plan_v7;
    const std::string planes_needle = "\"plane_sequences\":";
    rounded_v7.insert(
        rounded_v7.find(planes_needle),
        "\"rounds\": [],\n  ");
    const std::string rounded_v7_path =
        write_fixture(rounded_v7, "-v7-rounded");
    error.clear();
    ok &= expect(!load_ocs_plan_file(rounded_v7_path, loaded, error) &&
                 error.find("cannot contain global rounds") !=
                     std::string::npos,
                 "plan-v7 rejects mixed global and plane-local sequencing");

    std::string wrong_v7 = plan_v7;
    const std::string model = "\"independent_planes\"";
    wrong_v7.replace(wrong_v7.find(model), model.size(), "\"global_round\"");
    const std::string wrong_v7_path = write_fixture(wrong_v7, "-v7-wrong");
    error.clear();
    ok &= expect(!load_ocs_plan_file(wrong_v7_path, loaded, error) &&
                 error.find("execution_model=independent_planes") !=
                     std::string::npos,
                 "plan-v7 rejects an implicit or wrong execution model");

    std::string v5 = plan;
    const std::string needle = "\"version\": 6";
    v5.replace(v5.find(needle), needle.size(), "\"version\": 5");
    const std::string v5_path = write_fixture(v5, "-v5");
    error.clear();
    ok &= expect(!load_ocs_plan_file(v5_path, loaded, error) &&
                 error.find("version 6") != std::string::npos,
                 "plan-v5 is rejected by backend-v2");

    std::remove(path.c_str());
    std::remove(v5_path.c_str());
    std::remove(v7_path.c_str());
    std::remove(rounded_v7_path.c_str());
    std::remove(wrong_v7_path.c_str());
    if (ok) std::cout << "PASS: exact plan-v6/v7 identity" << std::endl;
    return ok ? 0 : 1;
}
