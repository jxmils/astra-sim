/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "HTSimNetworkApi.hh"
#include "astra-sim/common/Logging.hh"
#include "astra-sim/workload/HardwareResource.hh"
#include "astra-sim/workload/Workload.hh"
#include "common/CmdLineParser.hh"
#include "HTSimSession.hh"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <astra-network-analytical/common/EventQueue.h>
#include <astra-network-analytical/common/NetworkParser.h>
#include <astra-network-analytical/congestion_unaware/Helper.h>
#include <remote_memory_backend/analytical/AnalyticalRemoteMemory.hh>

using namespace HTSim;

namespace {
void print_backend_capabilities(const std::string& admission) {
    std::cout
        << "BACKEND_CAPABILITIES"
        << " backend_contract=1"
        << " concurrent_chakra_sends=1"
        << " chakra_send_admission=" << admission
        << " ocs_advance=transport_completion"
        << " planned_ocs_advance=transport_completion"
        << " planned_round_release=config_activation"
        << " ordinary_workload_barrier=nontransport_communicator_rendezvous"
        << " plan_round_barrier_discriminator=reserved_name_or_round_attr"
        << " dynamic_ocs_release=transport_completion"
        << " dynamic_ocs_estimated_release=0"
        << " ocs_plan_schema=6"
        << " ocs_independent_plane_schema=7"
        << " independent_plane_progress=1"
        << " independent_plane_barrier=plane_configuration_activation"
        << " exact_flow_identity=1"
        << " exact_stripe_identity=1"
        << " plan_lookup=exact"
        << " plan_fail_open=0"
        << " plan_end_audit=1"
        << " initial_ocs_state=cold"
        << " initial_ocs_reconfiguration=uniform"
        << " custom_link_latency=1"
        << " ocs_plane_latency=whole_path"
        << " serving_protocol=pass_deadline"
        << std::endl;
}

// Drop the "-1" placeholder cxxopts needs for an empty default.
std::vector<int> npu_id_list(const std::vector<int>& raw) {
    std::vector<int> ids;
    for (int id : raw) {
        if (id >= 0) {
            ids.push_back(id);
        }
    }
    return ids;
}

// Serving loop: the frontend (LLMServingSim) hands each rank one Chakra
// graph per iteration over stdin and reads the per-iteration report on
// stdout. This mirrors the loop in LLMServingSim's analytical frontend
// (network_frontend/analytical/congestion_unaware/main.cc) one for one,
// because the frontend's scheduler was tuned against it: generation-based
// re-ask suppression, "pass <tick>" arrival deadlines, "pass -1" for a
// state-changing pass, the bounded backstop, and the exact exit markers
// Controller.check_end parses. The one deliberate difference is idle time:
// with nothing in flight the analytical backend advances a 1 ms quantum
// per tick, whereas here the clock jumps straight to the earliest frontend
// deadline, so an admission never lands late by up to a quantum.
//
// Protocol (Python -> C++), one line per handshake after "Waiting":
//   <path>        run <path>.<rank>.et on this rank (and its managed ranks)
//   pass          nothing to run; suppress this rank until state changes
//   pass <tick>   same, plus the next known request arrival (ns)
//   pass -1       this pass changed scheduler state; re-open every rank
//   done          this instance is idle until exit
//   exit          shut down
int run_serving(HTSimSession& ht,
                std::vector<Sys*>& systems,
                const int npus_count,
                const std::vector<int>& start_npu_ids,
                const std::vector<int>& end_npu_ids) {
    auto log = AstraSim::LoggerFactory::get_logger("workload");

    // The systems each controller rank hands workloads to: every rank between
    // it and the next controller, minus the end ranks, which answer for
    // themselves.
    std::vector<std::vector<Sys*>> managed_systems(start_npu_ids.size());
    for (std::size_t idx = 0; idx < start_npu_ids.size(); ++idx) {
        const int npu_id = start_npu_ids[idx];
        const int upper_bound_id =
            (idx + 1 < start_npu_ids.size()) ? start_npu_ids[idx + 1] : npus_count;
        for (int sid = npu_id + 1; sid < upper_bound_id; ++sid) {
            if (sid < 0 || sid >= npus_count) {
                log->critical("Skipping invalid system id {} while building managed_systems", sid);
                continue;
            }
            if (std::find(end_npu_ids.begin(), end_npu_ids.end(), sid) != end_npu_ids.end()) {
                continue;
            }
            managed_systems[idx].push_back(systems[sid]);
        }
    }

    // Re-ask suppression: a rank that answered "pass" is not asked again
    // until some rank reports an iteration the frontend has not processed
    // (state_gen moves), a workload or "pass -1" re-opens everyone, or
    // simulated time reaches the deadline it supplied.
    constexpr long long kSuppressionBackstop = 1000;
    std::vector<long long> pass_gen(npus_count, -1);      // -1 = askable
    std::vector<long long> pass_deadline(npus_count, 0);  //  0 = no deadline
    std::vector<long long> last_reported_iter(npus_count, -1);
    long long state_gen = 0;
    long long unasked_rounds = 0;

    auto now_ns = [&]() -> long long {
        return static_cast<long long>(ht.get_time_ns());
    };
    auto askable = [&](int npu_id) {
        if (pass_gen[npu_id] < 0) {
            return true;
        }
        if (pass_gen[npu_id] != state_gen) {
            return true;
        }
        if (pass_deadline[npu_id] > 0 && now_ns() >= pass_deadline[npu_id]) {
            return true;
        }
        return false;
    };
    auto clear_suppression = [&]() {
        std::fill(pass_gen.begin(), pass_gen.end(), -1);
    };

    // One handshake with a finished rank. Returns false on "exit" or EOF.
    auto handshake = [&](int npu_id, const std::vector<Sys*>& managed) -> bool {
        AstraSim::Workload* workload = systems[npu_id]->workload;
        if (static_cast<long long>(workload->iteration) != last_reported_iter[npu_id]) {
            // An iteration the frontend has not processed yet: its decision
            // for every other rank may now differ.
            last_reported_iter[npu_id] = workload->iteration;
            ++state_gen;
        }
        // The Controller parses the line immediately before "Waiting"; flush
        // any buffered std::cout output (e.g. per-flow "Send flow" lines) so
        // it cannot land between the report and the prompt.
        std::cout.flush();
        workload->report();
        log->info("Waiting");

        std::string line;
        if (!std::getline(std::cin, line)) {
            log->critical("frontend closed stdin before \"exit\"");
            return false;
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.rfind("pass", 0) == 0) {
            const long long deadline = (line.size() > 5)
                ? std::strtoll(line.c_str() + 5, nullptr, 10)
                : 0;
            if (deadline < 0) {
                // "pass -1": the frontend changed scheduler state while
                // answering (a DP barrier round). Re-open every other rank;
                // this one waits for the rest of the round at the new
                // generation.
                ++state_gen;
                pass_gen[npu_id] = state_gen;
                pass_deadline[npu_id] = 0;
            } else {
                pass_gen[npu_id] = state_gen;
                pass_deadline[npu_id] = deadline;
            }
            return true;
        }

        // Anything else changed the frontend's state: re-ask everyone (the
        // instance's other TP ranks must pick the same batch up, a DP peer
        // must join the round it opens). The rank's old deadline is consumed
        // with it: left in place it would keep next_deadline in the past and
        // make every idle iteration clear the suppression of every rank.
        ++state_gen;
        pass_gen[npu_id] = -1;
        pass_deadline[npu_id] = 0;

        if (line == "exit") {
            return false;
        }
        if (line == "done") {
            workload->sleep_workload(managed);
            return true;
        }
        workload->add_workload(line, managed);
        return true;
    };

    bool exit_requested = false;
    while (!exit_requested) {
        bool asked_any = false;

        if (ht.astra_idle()) {
            // Nothing the frontend can observe is in flight (htsim's periodic
            // samplers may still be pending, but stepping them would only
            // burn simulated time). Only a frontend deadline moves the clock:
            // jump to the earliest one, which schedules a callback and makes
            // the loop step again until it fires.
            long long next_deadline = 0;
            for (int i = 0; i < npus_count; ++i) {
                if (pass_deadline[i] > 0 &&
                    (next_deadline == 0 || pass_deadline[i] < next_deadline)) {
                    next_deadline = pass_deadline[i];
                }
            }
            if (next_deadline > now_ns()) {
                ht.advance_to_ns(static_cast<double>(next_deadline));
            } else {
                clear_suppression();
            }
        } else {
            ht.step();
        }

        for (std::size_t idx = 0; idx < end_npu_ids.size() && !exit_requested; ++idx) {
            const int npu_id = end_npu_ids[idx];
            AstraSim::Workload* workload = systems[npu_id]->workload;
            if (!workload->is_sleep && workload->is_finished && askable(npu_id)) {
                asked_any = true;
                if (!handshake(npu_id, {})) {
                    exit_requested = true;
                }
            }
        }
        for (std::size_t idx = 0; idx < start_npu_ids.size() && !exit_requested; ++idx) {
            const int npu_id = start_npu_ids[idx];
            AstraSim::Workload* workload = systems[npu_id]->workload;
            if (!workload->is_sleep && workload->is_finished && askable(npu_id)) {
                asked_any = true;
                if (!handshake(npu_id, managed_systems[idx])) {
                    exit_requested = true;
                }
            }
        }

        if (asked_any) {
            unasked_rounds = 0;
        } else if (++unasked_rounds >= kSuppressionBackstop) {
            // Should be unreachable: a running graph always produces a
            // report, and an idle event list clears suppression. Kept so a
            // suppression bug degrades to a slow run instead of a hang.
            unasked_rounds = 0;
            clear_suppression();
            bool anyone_awake = false;
            for (int i = 0; i < npus_count; ++i) {
                if (!systems[i]->workload->is_sleep) {
                    anyone_awake = true;
                }
            }
            if (!anyone_awake) {
                // Every rank is "done" and no "exit" can ever be delivered.
                log->critical("all ranks asleep without \"exit\"; stopping");
                break;
            }
        }
    }

    // "exit" arrives on the first handshake after the frontend's last batch
    // completed, while other ranks of that instance can still have their
    // final callbacks in flight. Let them land so the end check below reports
    // the run's real end state, not the handshake instant.
    while (!ht.astra_idle()) {
        ht.step();
    }

    // Exit markers: Controller.check_end reads these exact lines.
    std::cout << "Checking Non-Exited Systems ..." << std::endl;
    bool done = true;
    for (int npu_id = 0; npu_id < npus_count; npu_id++) {
        if (!systems[npu_id]->workload->is_finished) {
            std::cout << "sys[" << npu_id << "] " << std::endl;
            done = false;
        }
    }
    std::cout << "---------------------------" << std::endl;
    std::cout << (done ? "All Request Has Been Exited" : "ERROR: Some Requests Remain") << std::endl;
    std::cout << "---------------------------" << std::endl;
    return done ? 0 : 1;
}
}  // namespace

int main(int argc, char* argv[]) {
    // Parse command line arguments
    auto cmd_line_parser = CmdLineParser(argv[0]);
    cmd_line_parser.get_options().add_options()(
        "htsim-proto", "HTSim Network Protocol [tcp]",
        cxxopts::value<HTSimProto>()->default_value("tcp"))(
        "chakra-send-admission", "Explicit Chakra send admission [serialized|concurrent]",
        cxxopts::value<std::string>())(
        "print-backend-capabilities", "Print backend capabilities and exit",
        cxxopts::value<bool>()->default_value("false"))(
        "serving", "Serving mode: take one graph per iteration per rank over stdin "
        "(LLMServingSim protocol) instead of running the initial graph to completion",
        cxxopts::value<bool>()->default_value("false"))(
        "start-npu-ids", "Serving mode: controller rank of each instance (comma list)",
        cxxopts::value<std::vector<int>>()->default_value("-1"))(
        "end-npu-ids", "Serving mode: last rank of each instance (comma list)",
        cxxopts::value<std::vector<int>>()->default_value("-1"))(
        "chakra-runtime-unit", "Unit of Chakra COMP node durations [us|ns]; us is the "
        "upstream ASTRA-sim convention, LLMServingSim writes ns",
        cxxopts::value<std::string>()->default_value("us"));
    cmd_line_parser.parse(argc, argv);

    if (std::getenv("ASTRA_CONCURRENT_SENDS") != nullptr) {
        std::cerr << "[Error] ASTRA_CONCURRENT_SENDS is no longer supported; use "
                  << "--chakra-send-admission=concurrent" << std::endl;
        return 2;
    }

    const auto chakra_send_admission =
        cmd_line_parser.get<std::string>("chakra-send-admission");
    if (chakra_send_admission == "concurrent") {
        AstraSim::set_chakra_send_admission(AstraSim::ChakraSendAdmission::Concurrent);
    } else if (chakra_send_admission == "serialized") {
        AstraSim::set_chakra_send_admission(AstraSim::ChakraSendAdmission::Serialized);
    } else {
        std::cerr << "[Error] invalid --chakra-send-admission value: "
                  << chakra_send_admission << std::endl;
        return 2;
    }

    print_backend_capabilities(chakra_send_admission);
    if (cmd_line_parser.get<bool>("print-backend-capabilities")) {
        return 0;
    }

    // Get command line arguments
    const auto workload_configuration = cmd_line_parser.get<std::string>("workload-configuration");
    const auto comm_group_configuration =
        cmd_line_parser.get<std::string>("comm-group-configuration");
    const auto system_configuration = cmd_line_parser.get<std::string>("system-configuration");
    const auto remote_memory_configuration =
        cmd_line_parser.get<std::string>("remote-memory-configuration");
    const auto network_configuration = cmd_line_parser.get<std::string>("network-configuration");
    const auto logging_configuration = cmd_line_parser.get<std::string>("logging-configuration");
    const auto num_queues_per_dim = cmd_line_parser.get<int>("num-queues-per-dim");
    const auto comm_scale = cmd_line_parser.get<double>("comm-scale");
    const auto injection_scale = cmd_line_parser.get<double>("injection-scale");
    const auto rendezvous_protocol = cmd_line_parser.get<bool>("rendezvous-protocol");
    const auto proto = cmd_line_parser.get<HTSimProto>("htsim-proto");
    const auto serving = cmd_line_parser.get<bool>("serving");
    const auto start_npu_ids = npu_id_list(cmd_line_parser.get<std::vector<int>>("start-npu-ids"));
    const auto end_npu_ids = npu_id_list(cmd_line_parser.get<std::vector<int>>("end-npu-ids"));

    AstraSim::LoggerFactory::init(logging_configuration);

    // Must precede Sys construction: it selects the per-iteration report
    // format and keeps a finished graph from retiring its rank.
    AstraSim::Workload::set_serving_mode(serving);

    const auto runtime_unit = cmd_line_parser.get<std::string>("chakra-runtime-unit");
    if (runtime_unit == "ns") {
        AstraSim::Workload::set_runtime_unit_ns(true);
    } else if (runtime_unit == "us") {
        AstraSim::Workload::set_runtime_unit_ns(false);
    } else {
        std::cerr << "[Error] invalid --chakra-runtime-unit value: " << runtime_unit
                  << " (us|ns)" << std::endl;
        return 2;
    }
    std::cout << "CHAKRA_RUNTIME_UNIT " << runtime_unit << std::endl;

    // Generate topology
    const auto network_parser = NetworkParser(network_configuration);
    const auto topology = construct_topology(network_parser);

    // Get topology information
    const auto npus_count = topology->get_npus_count();
    const auto npus_count_per_dim = topology->get_npus_count_per_dim();
    const auto dims_count = topology->get_dims_count();

    if (serving) {
        for (int id : start_npu_ids) {
            if (id < 0 || id >= npus_count) {
                std::cerr << "[Error] --start-npu-ids entry " << id
                          << " outside 0.." << npus_count - 1 << std::endl;
                return 2;
            }
        }
        for (int id : end_npu_ids) {
            if (id < 0 || id >= npus_count) {
                std::cerr << "[Error] --end-npu-ids entry " << id
                          << " outside 0.." << npus_count - 1 << std::endl;
                return 2;
            }
        }
    }

    // Set up Network API
    HTSimNetworkApi::set_topology(topology);
    auto completion_tracker = std::make_shared<CompletionTracker>(npus_count);
    HTSimNetworkApi::set_completion_tracker(completion_tracker);

    // Create ASTRA-sim related resources
    auto network_apis = std::vector<std::unique_ptr<HTSimNetworkApi>>();
    const auto memory_api =
        std::make_unique<Analytical::AnalyticalRemoteMemory>(remote_memory_configuration);
    auto systems = std::vector<Sys*>();

    auto queues_per_dim = std::vector<int>();
    for (auto i = 0; i < dims_count; i++) {
        queues_per_dim.push_back(num_queues_per_dim);
    }

    for (int i = 0; i < npus_count; i++) {
        // create network and system
        auto network_api = std::make_unique<HTSimNetworkApi>(i);
        auto* const system =
            new Sys(i, workload_configuration, comm_group_configuration, system_configuration,
                    memory_api.get(), network_api.get(), npus_count_per_dim, queues_per_dim,
                    injection_scale, comm_scale, rendezvous_protocol);

        // The remote-memory backend completes a request by scheduling on the
        // issuing rank's Sys, which it looks up by id; without this
        // registration MEM_LOAD/MEM_STORE nodes never complete. The MICRO
        // workloads the panel campaigns run have no memory nodes, which is
        // why this was never exercised here.
        memory_api->set_sys(i, system);

        // push back network and system
        network_apis.push_back(std::move(network_api));
        systems.push_back(system);
    }

    // Get HTSim opts
    int htsim_argc = 0;
    char** htsim_argv = NULL;
    for (int i = 0; i < argc; i++) {
        if (std::string(argv[i]) == "--htsim_opts") {
            htsim_argc = argc - i;
            htsim_argv = argv + i;
        }
    }

    // Report HTSim opts
    for (int i = 0; i < htsim_argc; i++) {
        std::cout << htsim_argv[i] << " ";
    }
    std::cout << std::endl;

    // Initialize HTSim session
    HTSimNetworkApi::htsim_info.nodes = npus_count;
    // Choose protocol
    auto& ht = HTSimSession::init(&HTSimNetworkApi::htsim_info, htsim_argc, htsim_argv, proto);

    // Initiate simulation
    for (int i = 0; i < npus_count; i++) {
        systems[i]->workload->fire();
    }

    if (serving) {
        ht.run_forever();
        const int rc = run_serving(ht, systems, npus_count, start_npu_ids, end_npu_ids);
        AstraSim::LoggerFactory::shutdown();
        ht.finish();
        return rc;
    }

    // run HTSim
    ht.run(&HTSimNetworkApi::htsim_info);

    // check if terminated properly
    if (!completion_tracker.get()->all_finished()) {
        std::cout << "Warning: Simulation timed out." << std::endl;
    }

    // terminate simulation
    AstraSim::LoggerFactory::shutdown();

    ht.finish();
    return 0;
}
