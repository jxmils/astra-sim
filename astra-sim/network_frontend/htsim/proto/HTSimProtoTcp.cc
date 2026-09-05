#include "HTSimProtoTcp.hh"
#include "HTSimSession.hh"
#include "OcsPlanLoader.hh"
#include <algorithm>
#include <climits>
#include <set>
#include <sstream>

// Adapted from HTSim main_tcp.cpp

#include "network.h"
#include "randomqueue.h"

#include "pipe.h"
#include "eventlist.h"
#include "logfile.h"
#include "mtcp.h"
#include "tcp.h"
#include "tcp_transfer.h"
#include "cbr.h"
#include "firstfit.h"

#include <cstring>
// Simulation params

#define PRINT_PATHS 0

#define PERIODIC 0
#include "main.h"

static FirstFit* ff = NULL;
static size_t subflow_count = 1;

#define USE_FIRST_FIT 0
#define FIRST_FIT_INTERVAL 100

namespace HTSim {

static void exit_error(char* progr) {
    std::cout << "Usage " << progr << " [UNCOUPLED(DEFAULT)|COUPLED_INC|FULLY_COUPLED|COUPLED_EPSILON] [epsilon][COUPLED_SCALABLE_TCP" << std::endl;
    exit(1);
}

static void print_path(std::ofstream &paths,const Route* rt){
    for (uint32_t i=1;i<rt->size()-1;i+=2){
        RandomQueue* q = (RandomQueue*)rt->at(i);
        if (q!=NULL) {
            paths << q->str() << " ";
        }
        else {
            paths << "NULL ";
        }
    }

    paths << std::endl;
}

// Impl constructor that loads config for session
HTSimProtoTcp::HTSimProtoTcp(const HTSim::tm_info* const tm, int argc, char** argv) {
    eventlist.setEndtime(timeFromSec(60));
    c = std::make_unique<Clock>(timeFromSec(50 / 100.), eventlist);
    no_of_nodes = tm->nodes;
    linkspeed = speedFromMbps((double)HOST_NIC);

    int i = 1;
    filename << "logout.dat";

    while (i<argc) {
        if (!strcmp(argv[i],"-o")){
            filename.str(std::string());
            filename << argv[i+1];
            i++;
        }
        else if (!strcmp(argv[i],"-sub")){
            subflow_count = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"-nodes")){
            no_of_nodes = atoi(argv[i+1]);
            std::cout << "no_of_nodes "<<no_of_nodes << std::endl;
            i++;
        } else if (!strcmp(argv[i],"-panel")){
            panel_kind = argv[i+1];
            i++;
        } else if (!strcmp(argv[i],"-extent")){
            // consumed via -nodes; retained for symmetry/clarity
            i++;
        } else if (!strcmp(argv[i],"-permute")){
            if (!strcmp(argv[i+1],"qtp8")) {
                static const int qtp8[8] = {0,1,4,5,7,2,3,6};
                panel_perm.assign(qtp8, qtp8+8);
            } else if (!strcmp(argv[i+1],"qtp_rows8")) {
                // Repeat the QTP-8 embedding independently in every physical
                // eight-node row. This supports RingRows at any N divisible
                // by eight while preserving the validated quartet/pair map.
                if (no_of_nodes % 8 != 0) {
                    std::cerr << "qtp_rows8 requires nodes divisible by 8"
                              << std::endl;
                    exit(1);
                }
                static const int qtp8m[8] = {0,1,4,5,7,2,3,6};
                panel_perm.assign(no_of_nodes, 0);
                for (int r = 0; r < no_of_nodes; r++)
                    panel_perm[r] = (r / 8) * 8 + qtp8m[r % 8];
            } else if (!strcmp(argv[i+1],"qtp64")) {
                // Row x column extension of qtp8 onto the 8x8 grid:
                // logical r = a + 4b + 8c + 32d  (dims [4,2,4,2]);
                // physical = qtp8[c+4d]*8 + qtp8[a+4b].
                static const int qtp8m[8] = {0,1,4,5,7,2,3,6};
                panel_perm.assign(64, 0);
                for (int r = 0; r < 64; r++) {
                    int a = r % 4, b = (r / 4) % 2, cc = (r / 8) % 4, dd = (r / 32) % 2;
                    panel_perm[r] = qtp8m[cc + 4 * dd] * 8 + qtp8m[a + 4 * b];
                }
            } else if (strcmp(argv[i+1],"identity")) {
                std::cerr << "Unknown -permute " << argv[i+1] << std::endl; exit(1);
            }
            i++;
        } else if (!strcmp(argv[i],"-nolog")){
            panel_nolog = true;
        } else if (!strcmp(argv[i],"-extents")){
            panel_extents.clear();
            const char* s = argv[i+1];
            int v = 0; bool any = false;
            for (; ; s++) {
                if (*s >= '0' && *s <= '9') { v = v*10 + (*s-'0'); any = true; }
                else { if (any) panel_extents.push_back(v); v = 0; any = false;
                       if (!*s) break; }
            }
            i++;
        } else if (!strcmp(argv[i],"-planes")){
            panel_planes = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"-linkGiBps")){
            panel_link_gibps = atof(argv[i+1]);
            panel_plane_gibps = atof(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"-planeGiBps")){
            panel_plane_gibps = atof(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"-latencyNs")){
            panel_latency = timeFromNs(atof(argv[i+1]));
            panel_plane_latency = panel_latency;
            i++;
        } else if (!strcmp(argv[i],"-policy")){
            if (!strcmp(argv[i+1],"static")) panel_policy = PanelPolicy::Static;
            else if (!strcmp(argv[i+1],"directpref")) panel_policy = PanelPolicy::DirectPref;
            else panel_policy = PanelPolicy::Adaptive;
            i++;
        } else if (!strcmp(argv[i],"-seed")){
            rng_seed = (unsigned)atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"-ocsplan")){
            ocs_plan_path = argv[i+1];
            ocs_plan_mode = true;
            i++;
        } else if (!strcmp(argv[i],"-ocs")){
            ocs_mode = true;
        } else if (!strcmp(argv[i],"-graph")){
            panel_graphfile = argv[i+1];
            i++;
        } else if (!strcmp(argv[i],"-redGBps")){
            // Owner reduction service rate (decimal GB/s of incoming
            // partial bytes a rank can reduce). 0/absent = unmodelled.
            ocs_red_Bps = atof(argv[i+1]) * 1e9;
            i++;
        } else if (!strcmp(argv[i],"-reconfNs")){
            ocs_reconf = timeFromNs(atof(argv[i+1]));
            i++;
        } else if (!strcmp(argv[i],"-maxwin")){
            nocc_maxwin = (uint64_t)atoll(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"-nocc")){
            nocc = true;
        } else if (!strcmp(argv[i],"-q")){
            queuesize_pkts = atoi(argv[i+1]);
            std::cout << "queuesize_pkts " << queuesize_pkts << std::endl;
            i++;
        } else if (!strcmp(argv[i], "-topo")) {
            topo_file = argv[i + 1];
            cout << "FatTree topology input file: " << topo_file << endl;
            i++;
        } else if (!strcmp(argv[i], "UNCOUPLED"))
            algo = UNCOUPLED;
        else if (!strcmp(argv[i], "COUPLED_INC"))
            algo = COUPLED_INC;
        else if (!strcmp(argv[i], "FULLY_COUPLED"))
            algo = FULLY_COUPLED;
        else if (!strcmp(argv[i], "COUPLED_TCP"))
            algo = COUPLED_TCP;
        else if (!strcmp(argv[i], "COUPLED_SCALABLE_TCP"))
            algo = COUPLED_SCALABLE_TCP;
        else if (!strcmp(argv[i], "COUPLED_EPSILON")) {
            algo = COUPLED_EPSILON;
            if (argc > i+1){
                epsilon = atof(argv[i+1]);
                i++;
            }
            printf("Using epsilon %f\n", epsilon);
        } else
            exit_error(argv[0]);

        i++;
    }
    if (rng_seed == 0) {
        rng_seed = (unsigned)time(NULL);
    }
    std::cout << "Using rng seed " << rng_seed << " (pass -seed " << rng_seed
              << " to replay)" << std::endl;
    std::cout << "Congestion control: " << (nocc ? "OFF (-nocc)" : "on") << std::endl;
    srand(rng_seed);

    std::cout << "Using subflow count " << subflow_count << std::endl;
    std::cout << "requested nodes " << no_of_nodes << std::endl;


    std::cout <<  "Using algo="<<algo<< " epsilon=" << epsilon << std::endl;
    // prepare the loggers

    std::cout << "Logging to " << filename.str() << std::endl;
    logfile = std::make_unique<Logfile>(filename.str(), eventlist);

#if PRINT_PATHS
    filename << ".paths";
    std::cout << "Logging path choices to " << filename.str() << std::endl;
    std::ofstream paths(filename.str().c_str());
    if (!paths){
        std::cout << "Can't open for writing paths file!"<< std::endl;
        exit(1);
    }
#endif

    logfile->setStartTime(timeFromSec(0));
    sinkLogger = std::make_unique<TcpSinkLoggerSampling>(timeFromMs(1000), eventlist);
    logfile->addLogger(*sinkLogger);

    tcpRtxScanner = std::make_unique<TcpRtxTimerScanner>(timeFromMs(10), eventlist);
    qlf = std::make_unique<QueueLoggerFactory>(logfile.get(), QueueLoggerFactory::LOGGER_SAMPLING, eventlist);
    qlf->set_sample_period(timeFromUs(1000.0));

#if USE_FIRST_FIT
    if (subflow_count==1){
        ff = new FirstFit(timeFromMs(FIRST_FIT_INTERVAL),eventlist);
    }
#endif

#ifdef FAT_TREE

if (!panel_kind.empty()) {
    if (panel_latency == 0) { panel_latency = timeFromNs(1000.0); panel_plane_latency = panel_latency; }
    PanelTopology::Base b;
    if (panel_kind == "ring1d") { b = PanelTopology::Base::Ring1D; if (panel_planes == 0) panel_planes = 2; }
    else if (panel_kind == "mesh2d") { b = PanelTopology::Base::Mesh2D; }
    else if (panel_kind == "torus2d") { b = PanelTopology::Base::Torus2D; }
    else if (panel_kind == "mesh3d") { b = PanelTopology::Base::Mesh3D; }
    else if (panel_kind == "torus3d") { b = PanelTopology::Base::Torus3D; }
    else if (panel_kind == "hybrid") { b = PanelTopology::Base::Torus2D; if (panel_planes == 0) panel_planes = 2; }
    else if (panel_kind == "fullswitch") { b = PanelTopology::Base::None; if (panel_planes == 0) panel_planes = 6; }
    else if (panel_kind == "ringrows") { b = PanelTopology::Base::RingRows; if (panel_planes == 0) panel_planes = 2; }
    else if (panel_kind == "custom") {
        // arbitrary physical graph from -graph; may contain non-endpoint
        // devices (HammingMesh row/column switches), routed shortest-path
        b = PanelTopology::Base::Custom;
        if (panel_graphfile.empty()) {
            std::cerr << "ERROR: -panel custom requires -graph <file>" << std::endl;
            exit(2);
        }
    }
    else { std::cerr << "Unknown -panel kind " << panel_kind << std::endl; exit(1); }
    panel_top = new PanelTopology(no_of_nodes, b, panel_planes,
                                  panel_link_gibps, panel_latency,
                                  panel_plane_gibps, panel_plane_latency,
                                  memFromPkt(queuesize_pkts), logfile.get(), &eventlist,
                                  panel_extents, panel_nolog, panel_graphfile);
    if (ocs_plan_mode) {
        if (!panel_top) { std::cerr << "-ocsplan requires -panel" << std::endl; exit(1); }
        load_ocs_plan();
    }
    if (ocs_mode && panel_planes > 0) {
        ocs_up_free.assign(panel_planes, std::vector<simtime_picosec>(no_of_nodes, 0));
        ocs_down_free.assign(panel_planes, std::vector<simtime_picosec>(no_of_nodes, 0));
        ocs_up_peer.assign(panel_planes, std::vector<int>(no_of_nodes, -1));
        ocs_down_peer.assign(panel_planes, std::vector<int>(no_of_nodes, -1));
    }
    std::cout << "Panel topology: " << panel_kind << " nodes " << no_of_nodes
              << " planes " << panel_planes << " linkGiBps " << panel_link_gibps
              << " OCS mode: " << (ocs_mode ? "on" : "off")
              << " reconfNs " << timeAsNs(ocs_reconf)
              << " policy " << (panel_policy == PanelPolicy::Static ? "static" :
                 panel_policy == PanelPolicy::DirectPref ? "directpref" : "adaptive")
              << std::endl;
} else if (topo_file) {

    FatTreeTopology* top_ = FatTreeTopology::load(topo_file, qlf.get(), eventlist,
    memFromPkt(queuesize_pkts), RANDOM, FAIR_PRIO);
    top = std::unique_ptr<FatTreeTopology>(top_);

    if (top->no_of_nodes() != no_of_nodes) {
        std::cerr << "Mismatch between connection matrix (" << no_of_nodes
        << " nodes) and topology (" << top->no_of_nodes() << " nodes)" << endl;
        exit(1);
    }
} else {
        top = std::make_unique<FatTreeTopology>(no_of_nodes, linkspeed, memFromPkt(queuesize_pkts), qlf.get(), &eventlist,ff,RANDOM,0);
    }
#endif

#ifdef OV_FAT_TREE
    top = std::make_unique<OversubscribedFatTreeTopology>(logfile.get(), &eventlist,ff);
#endif

#ifdef MH_FAT_TREE
    top = std::make_unique<MultihomedFatTreeTopology>(logfile.get(), &eventlist,ff);
#endif

#ifdef STAR
    top = std::make_unique<StarTopology>(logfile.get(), &eventlist,ff);
#endif

#ifdef BCUBE
    top = std::make_unique<BCubeTopology>(logfile.get(),&eventlist,ff);
    std::cout << "BCUBE " << K << std::endl;
#endif

#ifdef VL2
    top = std::make_unique<VL2Topology>(logfile.get(),&eventlist,ff);
#endif
    if (panel_top) {
        no_of_nodes = panel_top->no_of_nodes();
    } else {
        no_of_nodes = top->no_of_nodes();
    }
    std::cout << "actual nodes " << no_of_nodes << std::endl;

    net_paths = new vector<const Route*>**[no_of_nodes];
    is_dest = new int[no_of_nodes];

    for (uint32_t i=0;i<no_of_nodes;i++){
        is_dest[i] = 0;
        net_paths[i] = new vector<const Route*>*[no_of_nodes];
        for (uint32_t j = 0;j<no_of_nodes;j++)
            net_paths[i][j] = NULL;
    }

    if (ff)
        ff->net_paths = net_paths;
}

void HTSimProtoTcp::ocs_print_plan_audit(const char* status) {
    if (ocs_audit_printed) return;
    ocs_audit_printed = true;
    std::cout << "OCS_PLAN_AUDIT"
              << " expected_flows=" << ocs_expected_flows.size()
              << " started_flows=" << ocs_started_flows.size()
              << " completed_flows=" << ocs_completed_flows.size()
              << " expected_stripes=" << ocs_expected_stripes.size()
              << " started_stripes=" << ocs_started_stripes.size()
              << " completed_stripes=" << ocs_completed_stripes.size()
              << " expected_slots=" << ocs_expected_stripes.size()
              << " consumed_slots=" << ocs_consumed_slots.size()
              << " fallback_lookups=" << ocs_fallback_lookups
              << " ocs_advance_mode=transport_completion"
              << " expected_configurations=" << ocs_expected_configurations
              << " drained_configurations=" << ocs_drained_configurations
              << " config_drain_records=" << ocs_config_drain_records
              << " estimated_drain_events=" << ocs_estimated_drain_events
              << " premature_advances=" << ocs_premature_advances
              << " status=" << status << std::endl;
}

[[noreturn]] void HTSimProtoTcp::ocs_plan_fatal(const std::string& reason) {
    std::cerr << "OCS_PLAN_FATAL reason=" << reason << std::endl;
    ocs_print_plan_audit("FAIL");
    std::exit(3);
}

void HTSimProtoTcp::ocs_validate_active_stripe(
        const OcsPlanData::Asn& assignment,
        const OcsPlanData::Stripe& stripe,
        const std::pair<int, int>& slot,
        uint32_t physical_source,
        uint32_t physical_destination) {
    const int plane = slot.first;
    const int configuration = slot.second;
    if (plane < 0 || plane >= (int)ocs_cfgs.size() ||
        configuration < 0 || configuration >= (int)ocs_cfgs[plane].size()) {
        ocs_plan_fatal("stripe_slot_out_of_range");
    }
    if (stripe.plane != plane) ocs_plan_fatal("wrong_plane");

    const OcsCfg& configured = ocs_cfgs[plane][configuration];
    if (configured.round != assignment.round)
        ocs_plan_fatal("wrong_round_or_configuration");
    if (std::find(configured.matching.begin(), configured.matching.end(),
                  std::make_pair((int)physical_source,
                                 (int)physical_destination)) ==
        configured.matching.end()) {
        ocs_plan_fatal("flow_outside_installed_matching");
    }

    bool exact_circuit = false;
    for (const auto& circuit : configured.circuits) {
        if (circuit.stripe_uid == stripe.stripe_uid &&
            circuit.flow_uid == assignment.flow_uid &&
            circuit.src == (int)physical_source &&
            circuit.dst == (int)physical_destination &&
            circuit.bytes == stripe.bytes) {
            exact_circuit = true;
            break;
        }
    }
    if (!exact_circuit) ocs_plan_fatal("stripe_circuit_mismatch");

    switch (classify_ocs_slot_state(
            configuration, ocs_cur[plane], ocs_dark[plane])) {
        case OcsSlotState::Active: break;
        case OcsSlotState::Stale: ocs_plan_fatal("stale_past_round");
        case OcsSlotState::Future: ocs_plan_fatal("configuration_not_active");
        case OcsSlotState::Dark:
            ocs_plan_fatal("matching_reconfiguration_in_progress");
    }
}

void HTSimProtoTcp::load_ocs_plan() {
    OcsPlanData plan;
    std::string err;
    if (!load_ocs_plan_file(ocs_plan_path, plan, err)) {
        ocs_plan_fatal("plan_load_failed:" + err);
    }
    if (plan.endpoints != (int)no_of_nodes) {
        ocs_plan_fatal("endpoint_count_mismatch");
    }
    if (plan.planes != panel_planes) {
        ocs_plan_fatal("plane_count_mismatch");
    }
    ocs_plan_reconf_ns = plan.reconfiguration_ns;
    if (ocs_reconf != 10000) { ocs_plan_reconf_ns = timeAsNs(ocs_reconf); }
    ocs_initial_reconf = plan.initial_reconfiguration;
    ocs_plan_scheduled = plan.scheduled_bytes;
    ocs_cfgs.assign(plan.planes, {});
    ocs_cur.assign(plan.planes, 0);
    ocs_dark.assign(plan.planes, false);
    std::string identity_error;
    if (!build_ocs_plan_identity_index(plan, ocs_identity, identity_error)) {
        ocs_plan_fatal("identity_index_failed:" + identity_error);
    }
    for (const auto& assignment : plan.assignments_full) {
        ocs_expected_flows.insert(assignment.flow_uid);
        for (const auto& stripe : assignment.stripes)
            ocs_expected_stripes.insert(stripe.stripe_uid);
    }
    for (size_t k = 0; k < plan.configurations.size(); k++) {
        const OcsPlanData::Cfg& src = plan.configurations[k];
        OcsCfg oc; oc.round = src.round;
        oc.matching = src.matching;
        oc.force_reconf = src.force_reconf;
        oc.phase = src.phase;
        oc.synchronize = src.synchronize;
        int pl = src.plane;
        for (size_t q = 0; q < src.circuits.size(); q++) {
            int s = src.circuits[q].src;
            int d = src.circuits[q].dst;
            uint64_t b = src.circuits[q].bytes;
            OcsCircuit circuit;
            circuit.src = s; circuit.dst = d; circuit.bytes = b;
            circuit.flow_uid = src.circuits[q].flow_uid;
            circuit.stripe_uid = src.circuits[q].stripe_uid;
            oc.circuits.push_back(circuit);
        }
        oc.remaining = (int)oc.circuits.size();
        ocs_cfgs[pl].push_back(oc);
        ocs_expected_configurations++;
    }
    s_self = this;
    std::cout << "OCS plan loaded: planes " << plan.planes
              << " rounds " << plan.rounds
              << " reconf_ns " << ocs_plan_reconf_ns
              << " scheduled_bytes " << ocs_plan_scheduled << std::endl;
}

static void ocs_advance_cb(void* arg);   // defined below

void HTSimProtoTcp::ocs_install_next(int plane) {
    ocs_install_next_uncharged(plane, false);   // reconfig counted at drain time
}

void HTSimProtoTcp::ocs_install_next_uncharged(int plane, bool /*counted*/) {
    const int current_index = ocs_cur[plane];
    if (current_index < 0 || current_index >= (int)ocs_cfgs[plane].size())
        ocs_plan_fatal("configuration_advance_out_of_range");
    OcsCfg& current = ocs_cfgs[plane][current_index];
    if (!current.drained ||
        current.started != (int)current.circuits.size() ||
        current.completed != (int)current.circuits.size()) {
        ocs_premature_advances++;
        ocs_plan_fatal("premature_configuration_advance");
    }
    // red gate: hold the first unfold configuration on this plane until
    // every owner's reduction engine has drained the partials it must
    // combine. Applied AT MOST ONCE per plane: re-checking on each retry
    // lets a still-advancing ready time reschedule in a tight loop, which
    // exhausts the event queue (observed as std::bad_alloc).
    if (ocs_red_Bps > 0.0) {
        if ((int)red_gate_done.size() <= plane) red_gate_done.resize(plane + 1, 0);
        int nxt = ocs_cur[plane] + 1;
        if (!red_gate_done[plane] && nxt < (int)ocs_cfgs[plane].size() &&
            ocs_cfgs[plane][nxt].phase == 2) {
            double now_ns = timeAsNs(eventlist.now()), ready = 0.0;
            for (size_t r = 0; r < red_busy_until_ns.size(); r++)
                if (red_busy_until_ns[r] > ready) ready = red_busy_until_ns[r];
            red_gate_done[plane] = 1;
            if (ready > now_ns + 1.0) {
                ocs_red_wait_ns += (ready - now_ns);
                ocs_dark[plane] = true;
                std::pair<HTSimProtoTcp*, int>* arg =
                    new std::pair<HTSimProtoTcp*, int>(this, plane);
                HTSimSession::instance().schedule_astra_event(
                    ready - now_ns, &ocs_advance_cb, arg);
                return;
            }
        }
    }
    ocs_print_config_drain(plane, current_index, eventlist.now());
    ocs_dark[plane] = false;
    ocs_cur[plane]++;
    ocs_plan_rounds_done++;
    ocs_cfg_times[std::make_pair(plane, ocs_cur[plane])].first =
        timeAsNs(eventlist.now());
}

static void ocs_advance_cb(void* arg) {
    // arg encodes (impl*, plane) via a heap pair
    std::pair<HTSimProtoTcp*, int>* pp = (std::pair<HTSimProtoTcp*, int>*)arg;
    pp->first->ocs_install_next(pp->second);
    delete pp;
}

bool HTSimProtoTcp::matching_changed(const OcsCfg& a, const OcsCfg& b) {
    // Reconfiguration is charged only when the installed matching actually
    // changes (mirrors OcsSwitch::configuration_changed): compare the
    // (src,dst) pair sets, ignoring byte quotas.
    std::set<std::pair<int,int>> sa, sb;
    if (!a.matching.empty())
        sa.insert(a.matching.begin(), a.matching.end());
    else
        for (size_t i = 0; i < a.circuits.size(); i++)
            sa.insert({a.circuits[i].src, a.circuits[i].dst});
    if (!b.matching.empty())
        sb.insert(b.matching.begin(), b.matching.end());
    else
        for (size_t i = 0; i < b.circuits.size(); i++)
            sb.insert({b.circuits[i].src, b.circuits[i].dst});
    return sa != sb;
}

// The installed configuration's exact stripes have all reported sender-side
// HTSim transport completion. Only now may the matching be retired.
void HTSimProtoTcp::ocs_advance_after_drain(int plane) {
    int cfg = ocs_cur[plane];
    OcsCfg& current = ocs_cfgs[plane][cfg];
    if (!current.drained ||
        current.started != (int)current.circuits.size() ||
        current.completed != (int)current.circuits.size()) {
        ocs_premature_advances++;
        ocs_plan_fatal("premature_configuration_advance");
    }
    if (cfg + 1 >= (int)ocs_cfgs[plane].size()) return;
    bool changed = ocs_cfgs[plane][cfg + 1].force_reconf ||
                   matching_changed(ocs_cfgs[plane][cfg], ocs_cfgs[plane][cfg + 1]);
    if (changed) ocs_reconfigs++;
    if (changed && ocs_plan_reconf_ns > 0) {
        ocs_dark[plane] = true;
        std::pair<HTSimProtoTcp*, int>* arg = new std::pair<HTSimProtoTcp*, int>(this, plane);
        HTSimSession::instance().schedule_astra_event(ocs_plan_reconf_ns, &ocs_advance_cb, arg);
    } else {
        ocs_install_next_uncharged(plane, changed);
    }
}

void HTSimProtoTcp::ocs_print_config_drain(
        int plane, int configuration, simtime_picosec advance_time) {
    OcsCfg& current = ocs_cfgs[plane][configuration];
    if (current.drain_reported) return;
    if (!current.drained || !current.has_completion ||
        current.started != (int)current.circuits.size() ||
        current.completed != (int)current.circuits.size() ||
        advance_time < current.last_complete) {
        ocs_premature_advances++;
        ocs_plan_fatal("invalid_configuration_drain_record");
    }
    const bool has_next = configuration + 1 < (int)ocs_cfgs[plane].size();
    const bool changed = has_next &&
        (ocs_cfgs[plane][configuration + 1].force_reconf ||
         matching_changed(current, ocs_cfgs[plane][configuration + 1]));
    std::ostringstream record;
    record << "OCS_CONFIG_DRAIN"
           << " plane=" << plane
           << " config=" << configuration
           << " round=" << current.round
           << " expected_stripes=" << current.circuits.size()
           << " started_stripes=" << current.started
           << " completed_stripes=" << current.completed
           << " last_start_ns=" << timeAsNs(current.last_start)
           << " first_complete_ns=" << timeAsNs(current.first_complete)
           << " last_complete_ns=" << timeAsNs(current.last_complete)
           << " advance_ns=" << timeAsNs(advance_time)
           << " reconfiguration_ns="
           << (changed ? ocs_plan_reconf_ns : 0.0)
           << " status=PASS\n";
    std::cout << record.str();
    std::cout.flush();
    current.drain_reported = true;
    ocs_config_drain_records++;
}

void HTSimProtoTcp::ocs_drain_reached(int plane) {
    int cfg = ocs_cur[plane];
    OcsCfg& current = ocs_cfgs[plane][cfg];
    if (current.drained) ocs_plan_fatal("configuration_drained_more_than_once");
    if (current.started != (int)current.circuits.size() ||
        current.completed != (int)current.circuits.size()) {
        ocs_premature_advances++;
        ocs_plan_fatal("configuration_drained_before_transport_completion");
    }
    ocs_cfg_times[std::make_pair(plane, cfg)].second = timeAsNs(eventlist.now());
    current.drained = true;
    ocs_drained_configurations++;
    if (cfg + 1 >= (int)ocs_cfgs[plane].size())
        ocs_print_config_drain(plane, cfg, eventlist.now());
    if (!current.synchronize) {
        ocs_advance_after_drain(plane);
        return;
    }

    const int round = current.round;
    std::vector<int> participants;
    for (int other = 0; other < (int)ocs_cfgs.size(); other++) {
        bool participates = false;
        for (size_t index = 0; index < ocs_cfgs[other].size(); index++) {
            const OcsCfg& configured = ocs_cfgs[other][index];
            if (configured.round == round && configured.synchronize) {
                participates = true;
                break;
            }
        }
        if (!participates) continue;
        int other_cfg = ocs_cur[other];
        if (other_cfg >= (int)ocs_cfgs[other].size()) return;
        OcsCfg& candidate = ocs_cfgs[other][other_cfg];
        if (candidate.round != round) return;
        participants.push_back(other);
        if (!candidate.synchronize || !candidate.drained) return;
    }
    for (size_t index = 0; index < participants.size(); index++)
        ocs_advance_after_drain(participants[index]);
}

void HTSimProtoTcp::flow_done(int flow_id) {
    if (ocs_plan_mode) {
        auto flow = ocs_runtime_flow_uid.find(flow_id);
        if (flow == ocs_runtime_flow_uid.end())
            ocs_plan_fatal("completion_for_unknown_runtime_flow");
        if (!ocs_started_flows.count(flow->second) ||
            !ocs_completed_flows.insert(flow->second).second) {
            ocs_plan_fatal("flow_completed_before_start_or_more_than_once");
        }
        ocs_runtime_flow_uid.erase(flow);
        if (ocs_runtime_stripe_uid.count(flow_id))
            ocs_note_stripe_completed(flow_id);
    }
    ocs_flow_cfg.erase(flow_id);
}

HTSimProtoTcp* HTSimProtoTcp::s_self = NULL;

// Stripe sub-flow completion: forward the logical (master) completion to
// ASTRA only when every stripe has finished. The sink callback can fire more
// than once per flow, so completions are counted at most once per direction.
static std::map<int,int> stripe_sub_state;   // sub tag -> bit0 send, bit1 recv

void HTSimProtoTcp::stripe_finish_send(int, int, int, int tag) {
    HTSimProtoTcp* self = s_self;
    if (!self) return;
    auto mi = self->stripe_sub2master.find(tag);
    if (mi == self->stripe_sub2master.end()) return;
    int& st = stripe_sub_state[tag];
    if (st & 1) return;
    st |= 1;
    self->ocs_note_stripe_completed(tag);
    int master = mi->second;
    if (st == 3) { stripe_sub_state.erase(tag); self->stripe_sub2master.erase(mi); }
    auto sm = self->stripe_masters.find(master);
    if (sm == self->stripe_masters.end()) return;
    StripeMaster& m = sm->second;
    if (--m.pending_send == 0 && !m.sent_fwd) {
        m.sent_fwd = true;
        HTSimSession::flow_finish_send(m.src, m.dst, (int)m.total, master);
    }
    if (m.sent_fwd && m.recv_fwd) self->stripe_masters.erase(sm);
}

void HTSimProtoTcp::ocs_note_stripe_completed(int flow_id) {
    auto configured = ocs_flow_cfg.find(flow_id);
    if (configured == ocs_flow_cfg.end())
        ocs_plan_fatal("stripe_completion_has_no_exact_slot");
    const int plane = configured->second.first;
    const int configuration = configured->second.second;
    if (plane < 0 || plane >= (int)ocs_cfgs.size() ||
        configuration < 0 || configuration >= (int)ocs_cfgs[plane].size())
        ocs_plan_fatal("stripe_completion_slot_out_of_range");
    if (classify_ocs_slot_state(
            configuration, ocs_cur[plane], ocs_dark[plane]) !=
        OcsSlotState::Active) {
        ocs_premature_advances++;
        ocs_plan_fatal("stripe_completed_after_configuration_retired");
    }
    auto stripe = ocs_runtime_stripe_uid.find(flow_id);
    if (stripe == ocs_runtime_stripe_uid.end())
        ocs_plan_fatal("completion_for_unknown_runtime_stripe");
    if (!ocs_started_stripes.count(stripe->second) ||
        !ocs_completed_stripes.insert(stripe->second).second) {
        ocs_plan_fatal("stripe_completed_before_start_or_more_than_once");
    }
    ocs_runtime_stripe_uid.erase(stripe);
    OcsCfg& current = ocs_cfgs[plane][configuration];
    current.completed++;
    if (current.completed > current.started ||
        current.completed > (int)current.circuits.size())
        ocs_plan_fatal("configuration_completed_too_many_stripes");
    const simtime_picosec now = eventlist.now();
    if (!current.has_completion) {
        current.first_complete = now;
        current.has_completion = true;
    }
    current.last_complete = now;
    ocs_flow_cfg.erase(configured);
    if (current.started == (int)current.circuits.size() &&
        current.completed == (int)current.circuits.size())
        ocs_drain_reached(plane);
}

void HTSimProtoTcp::stripe_finish_recv(int, int, int, int tag) {
    HTSimProtoTcp* self = s_self;
    if (!self) return;
    auto mi = self->stripe_sub2master.find(tag);
    if (mi == self->stripe_sub2master.end()) return;
    int& st = stripe_sub_state[tag];
    if (st & 2) return;
    st |= 2;
    int master = mi->second;
    if (st == 3) { stripe_sub_state.erase(tag); self->stripe_sub2master.erase(mi); }
    auto sm = self->stripe_masters.find(master);
    if (sm == self->stripe_masters.end()) return;
    StripeMaster& m = sm->second;
    if (--m.pending_recv == 0 && !m.recv_fwd) {
        m.recv_fwd = true;
        HTSimSession::flow_finish_recv(m.src, m.dst, (int)m.total, master);
    }
    if (m.sent_fwd && m.recv_fwd) self->stripe_masters.erase(sm);
}

// Select among panel route candidates per the routing policy, mirroring the
// analytical Hybrid2D::route semantics: cost = sum(latency) + max over hops of
// (reserved_bytes + s)/B; evaluation order direct -> preferred plane -> other
// planes with strict-improvement, so ties favour direct then lower-preference;
// reservations are placed on every hop of the chosen route at injection.
static PanelTopology::Candidate* panel_route_select(
        std::vector<PanelTopology::Candidate>* cands, uint64_t s,
        uint32_t src, uint32_t dst, int nplanes, bool include_queue,
        bool directpref, double pref_factor) {
    auto cost_ns = [&](const PanelTopology::Candidate& cd) {
        double lat_ns = (double)cd.latency_sum / 1000.0;
        double ser_ns = 0.0;
        for (size_t k = 0; k < cd.hop_queues.size(); k++) {
            LedgerQueue* q = cd.hop_queues[k];
            double Bpns = (double)q->link_bitrate() / 8.0 / 1e9;   // bytes per ns
            double queued = include_queue ? (double)q->reserved_bytes() : 0.0;
            double t = (queued + (double)s) / Bpns;
            if (t > ser_ns) ser_ns = t;
        }
        return lat_ns + ser_ns;
    };
    // ordering: direct (if present) first, then preferred plane, then rest
    std::vector<int> order;
    int first_plane_idx = -1;
    for (size_t i = 0; i < cands->size(); i++) {
        if (!(*cands)[i].is_plane) order.push_back((int)i);
        else if (first_plane_idx < 0) first_plane_idx = (int)i;
    }
    if (first_plane_idx >= 0 && nplanes > 0) {
        int pref = (int)((src + dst) % (uint32_t)nplanes);
        order.push_back(first_plane_idx + pref);
        for (int p = 0; p < nplanes; p++)
            if (p != pref) order.push_back(first_plane_idx + p);
    }
    int best = order[0];
    double best_cost = cost_ns((*cands)[best]);
    int best_plane = -1; double best_plane_cost = 0.0;
    for (size_t oi = 1; oi < order.size(); oi++) {
        double cst = cost_ns((*cands)[order[oi]]);
        if ((*cands)[order[oi]].is_plane &&
            (best_plane < 0 || cst < best_plane_cost)) {
            best_plane = order[oi]; best_plane_cost = cst;
        }
        if (cst < best_cost) { best_cost = cst; best = order[oi]; }
    }
    if (directpref && !(*cands)[order[0]].is_plane && best_plane >= 0) {
        double direct_cost = cost_ns((*cands)[order[0]]);
        best = (direct_cost <= pref_factor * best_plane_cost) ? order[0] : best_plane;
    }
    return &(*cands)[best];
}

void HTSimProtoTcp::ocs_note_started(int flow_id, uint64_t bytes,
                                     linkspeed_bps plane_rate) {
    std::map<int, std::pair<int,int>>::iterator it = ocs_flow_cfg.find(flow_id);
    if (it == ocs_flow_cfg.end()) ocs_plan_fatal("optical_start_has_no_slot");
    auto stripe = ocs_runtime_stripe_uid.find(flow_id);
    if (stripe == ocs_runtime_stripe_uid.end())
        ocs_plan_fatal("optical_start_has_no_stripe_uid");
    if (!ocs_expected_stripes.count(stripe->second) ||
        !ocs_started_stripes.insert(stripe->second).second) {
        ocs_plan_fatal("stripe_started_more_than_once_or_unknown");
    }
    int pl = it->second.first, cfgi = it->second.second;
    if (pl < 0 || pl >= (int)ocs_cfgs.size() ||
        cfgi < 0 || cfgi >= (int)ocs_cfgs[pl].size())
        ocs_plan_fatal("optical_start_slot_out_of_range");
    switch (classify_ocs_slot_state(cfgi, ocs_cur[pl], ocs_dark[pl])) {
        case OcsSlotState::Active: break;
        case OcsSlotState::Stale: ocs_plan_fatal("stale_past_round");
        case OcsSlotState::Future: ocs_plan_fatal("configuration_not_active");
        case OcsSlotState::Dark:
            ocs_plan_fatal("matching_reconfiguration_in_progress");
    }
    OcsCfg& oc = ocs_cfgs[pl][cfgi];
    oc.started++;
    oc.last_start = eventlist.now();
    if (oc.started > (int)oc.circuits.size())
        ocs_plan_fatal("configuration_started_too_many_stripes");
    // Owner reduction service: a fold shard becomes reducible when it
    // finishes arriving; the owner's engine then consumes it at
    // ocs_red_Bps. Service is a busy-until ledger per owner, so
    // reduction overlaps arrivals rather than starting after them. This
    // pre-existing reduction estimate does not control OCS advancement.
    if (ocs_red_Bps > 0.0 && oc.phase == 1) {
        std::map<int, std::pair<int,uint64_t>>::iterator di =
            red_flow_dst.find(flow_id);
        if (di != red_flow_dst.end()) {
            int owner = di->second.first;
            if ((int)red_busy_until_ns.size() <= owner)
                red_busy_until_ns.resize(owner + 1, 0.0);
            simtime_picosec estimated_red_arrival = eventlist.now()
                + (simtime_picosec)((double)bytes * 8.0 * 1e12 /
                                    (double)plane_rate)
                + (simtime_picosec)(2.0 * 1500.0 * 8.0 * 1e12 /
                                    (double)plane_rate);
            double arrive_ns = timeAsNs(estimated_red_arrival);
            double start_ns = std::max(red_busy_until_ns[owner], arrive_ns);
            red_busy_until_ns[owner] =
                start_ns + (double)bytes / ocs_red_Bps * 1e9;
        }
    }
}

// Schedule_htsim_event creates a new connection and schedules it in HTSim.
// Adapted from main connections loop
void HTSimProtoTcp::schedule_htsim_event(FlowInfo flow, int flow_id) {
    auto src = flow.src;
    auto dst = flow.dst;
    auto msg_size = flow.size;
    double start = eventlist.now();
    connID++;

    std::vector<PanelTopology::Candidate>* panel_cands = NULL;
    PanelTopology::Candidate* panel_choice = NULL;
    simtime_picosec panel_flow_delay = 0;
    int ocs_forced_plane = -2;   // -2 = not plan mode; -1 = DIRECT; >=0 plane
    // Plan mode is exact-only. Internal stripe subflows are admitted solely
    // through the runtime identity installed by their already-validated master.
    auto internal_stripe = ocs_runtime_stripe_uid.find(flow_id);
    if (panel_top && ocs_plan_mode &&
        internal_stripe != ocs_runtime_stripe_uid.end()) {
        auto configured = ocs_flow_cfg.find(flow_id);
        if (configured == ocs_flow_cfg.end())
            ocs_plan_fatal("internal_stripe_has_no_exact_slot");
        const int plane = configured->second.first;
        const int configuration = configured->second.second;
        if (plane < 0 || plane >= (int)ocs_cfgs.size() ||
            configuration < 0 || configuration >= (int)ocs_cfgs[plane].size())
            ocs_plan_fatal("internal_stripe_slot_out_of_range");
        switch (classify_ocs_slot_state(
                configuration, ocs_cur[plane], ocs_dark[plane])) {
            case OcsSlotState::Active: break;
            case OcsSlotState::Stale: ocs_plan_fatal("stale_past_round");
            case OcsSlotState::Future: ocs_plan_fatal("configuration_not_active");
            case OcsSlotState::Dark:
                ocs_plan_fatal("matching_reconfiguration_in_progress");
        }
        ocs_forced_plane = plane;
        ocs_plan_transmitted += flow.size;
    } else if (panel_top && ocs_plan_mode) {
        uint32_t ps = (panel_perm.empty() || (size_t)flow.src >= panel_perm.size())
            ? (uint32_t)flow.src : (uint32_t)panel_perm[flow.src];
        uint32_t pd = (panel_perm.empty() || (size_t)flow.dst >= panel_perm.size())
            ? (uint32_t)flow.dst : (uint32_t)panel_perm[flow.dst];
        if (flow.flow_uid.empty()) ocs_plan_fatal("missing_flow_uid");
        if (!ocs_expected_flows.count(flow.flow_uid))
            ocs_plan_fatal("unknown_or_extra_flow_uid:" + flow.flow_uid);
        if (ocs_started_flows.count(flow.flow_uid))
            ocs_plan_fatal("flow_uid_consumed_twice:" + flow.flow_uid);

        OcsPlanData::Asn assignment;
        if (!consume_ocs_assignment(ocs_identity, flow.flow_uid, assignment))
            ocs_plan_fatal("flow_uid_not_available:" + flow.flow_uid);
        if (assignment.src != (int)ps) ocs_plan_fatal("wrong_src");
        if (assignment.dst != (int)pd) ocs_plan_fatal("wrong_dst");
        if (assignment.bytes != (uint64_t)flow.size)
            ocs_plan_fatal("wrong_byte_count");
        if (assignment.tag != flow.plan_tag) ocs_plan_fatal("wrong_tag");
        if (ocs_runtime_flow_uid.count(flow_id))
            ocs_plan_fatal("duplicate_runtime_flow_id");

        std::vector<std::pair<int, int>> exact_slots;
        for (const auto& stripe : assignment.stripes) {
            if (stripe.stripe_uid.empty()) ocs_plan_fatal("missing_stripe_uid");
            if (!ocs_expected_stripes.count(stripe.stripe_uid))
                ocs_plan_fatal("unknown_stripe_uid:" + stripe.stripe_uid);
            std::pair<int, int> slot(-1, -1);
            if (!consume_ocs_stripe_slot(
                    ocs_identity, stripe.stripe_uid, slot)) {
                ocs_plan_fatal("stripe_uid_consumed_twice_or_missing:" +
                               stripe.stripe_uid);
            }
            if (!ocs_consumed_slots.insert(stripe.stripe_uid).second)
                ocs_plan_fatal("stripe_slot_consumed_twice:" + stripe.stripe_uid);
            ocs_validate_active_stripe(assignment, stripe, slot, ps, pd);
            exact_slots.push_back(slot);
        }

        ocs_started_flows.insert(assignment.flow_uid);
        ocs_runtime_flow_uid[flow_id] = assignment.flow_uid;
        red_flow_phase[flow_id] = assignment.phase;

        if (!assignment.is_direct && assignment.stripes.size() > 1) {
            // Striped optical transfer: split into per-plane sub-flows; ASTRA
            // sees completion when the last stripe drains.
            StripeMaster m; m.src = (int)src; m.dst = (int)dst;
            m.total = flow.size;
            m.pending_send = m.pending_recv = (int)assignment.stripes.size();
            stripe_masters[flow_id] = m;
            for (size_t si = 0; si < assignment.stripes.size(); si++) {
                const auto& stripe = assignment.stripes[si];
                const std::pair<int, int>& slot = exact_slots[si];
                int sub = stripe_next_tag++;
                stripe_sub2master[sub] = flow_id;
                ocs_runtime_stripe_uid[sub] = stripe.stripe_uid;
                HTSim::FlowInfo sf = flow;
                sf.size = stripe.bytes;
                sf.flow_uid = stripe.stripe_uid;
                std::cout << "OCS_IDENTITY flow_uid=" << assignment.flow_uid
                          << " stripe_uid=" << stripe.stripe_uid
                          << " plane=" << slot.first
                          << " round=" << ocs_cfgs[slot.first][slot.second].round
                          << " lookup=exact"
                          << std::endl;
                ocs_flow_cfg[sub] = slot;
                schedule_htsim_event(sf, sub);
            }
            return;   // master flow is virtual; stripes carry the bytes
        }

        if (assignment.is_direct) {
            ocs_forced_plane = -1;
            std::cout << "OCS_IDENTITY flow_uid=" << assignment.flow_uid
                      << " stripe_uid=- plane=-1 round=-1 lookup=exact"
                      << std::endl;
        } else {
            if (assignment.stripes.size() != 1 || exact_slots.size() != 1)
                ocs_plan_fatal("optical_flow_has_invalid_stripe_count");
            const auto& stripe = assignment.stripes.front();
            const auto& slot = exact_slots.front();
            const int pl = slot.first;
            const int cfgi = slot.second;
            ocs_runtime_stripe_uid[flow_id] = stripe.stripe_uid;
            std::cout << "OCS_IDENTITY flow_uid=" << assignment.flow_uid
                      << " stripe_uid=" << stripe.stripe_uid
                      << " plane=" << pl
                      << " round=" << ocs_cfgs[pl][cfgi].round
                      << " lookup=exact" << std::endl;
            red_flow_dst[flow_id] = std::make_pair((int)pd, (uint64_t)flow.size);
            ocs_flow_cfg[flow_id] = slot;
            ocs_forced_plane = pl;
            ocs_plan_transmitted += flow.size;
        }
    }
    uint32_t phys_src = (panel_perm.empty() || src >= panel_perm.size())
        ? (uint32_t)src : (uint32_t)panel_perm[src];
    uint32_t phys_dst = (panel_perm.empty() || dst >= panel_perm.size())
        ? (uint32_t)dst : (uint32_t)panel_perm[dst];
    if (panel_top) {
        panel_cands = panel_top->get_candidates(phys_src, phys_dst);
        if ((ocs_mode || ocs_plan_mode) && panel_top->planes() > 0) {
            // Evaluate: direct via ledger cost; each plane via lease cost
            //   T_O = wait + T_r(unless same-pair reuse) + 2L + s/B.
            if (ocs_forced_plane >= -1) {
            // plan decides: DIRECT -> the direct candidate; else the plane's
            // 2-hop route. No policy, no leasing; gating already done above.
            int want = ocs_forced_plane;
            int idx = -1;
            for (size_t ci = 0; ci < panel_cands->size(); ci++) {
                PanelTopology::Candidate& cd = (*panel_cands)[ci];
                if (want < 0 && !cd.is_plane) { idx = (int)ci; break; }
                if (want >= 0 && cd.is_plane && cd.plane == want) { idx = (int)ci; break; }
            }
            assert(idx >= 0);
            panel_choice = &(*panel_cands)[idx];
            if (ocs_plan_mode && panel_choice->is_plane) {
                ocs_note_started(flow_id, msg_size,
                                 panel_choice->hop_queues[0]->link_bitrate());
            }
            if (!panel_choice->is_plane) {
                for (size_t k = 0; k < panel_choice->hop_queues.size(); k++)
                    panel_choice->hop_queues[k]->reserve_bytes(msg_size);
                // direct fold shard: charge the owner's reduction engine
                std::map<int,int>::iterator pi = red_flow_phase.find(flow_id);
                if (ocs_red_Bps > 0.0 && pi != red_flow_phase.end() &&
                    pi->second == 1 && !panel_choice->hop_queues.empty()) {
                    double rate = (double)panel_choice->hop_queues[0]->link_bitrate()
                                  / 8.0;
                    if ((int)red_busy_until_ns.size() <= (int)phys_dst)
                        red_busy_until_ns.resize(phys_dst + 1, 0.0);
                    double arrive = timeAsNs(eventlist.now())
                                    + (double)msg_size / rate * 1e9;
                    double st = std::max(red_busy_until_ns[phys_dst], arrive);
                    red_busy_until_ns[phys_dst] =
                        st + (double)msg_size / ocs_red_Bps * 1e9;
                }
            }
        } else {
        simtime_picosec now_ps = eventlist.now();
            int best = -1; double best_cost = 0; bool best_reuse = false;
            simtime_picosec best_start = 0;
            double direct_cost = -1; int direct_idx = -1;
            for (size_t ci = 0; ci < panel_cands->size(); ci++) {
                PanelTopology::Candidate& cd = (*panel_cands)[ci];
                double cost;
                bool reuse = false; simtime_picosec tstart = now_ps;
                if (!cd.is_plane) {
                    double ser = 0;
                    for (size_t k = 0; k < cd.hop_queues.size(); k++) {
                        LedgerQueue* q = cd.hop_queues[k];
                        double Bpns = (double)q->link_bitrate() / 8.0 / 1e9;
                        double queued = (panel_policy != PanelPolicy::Static)
                                            ? (double)q->reserved_bytes() : 0.0;
                        double t = (queued + (double)msg_size) / Bpns;
                        if (t > ser) ser = t;
                    }
                    cost = (double)cd.latency_sum / 1000.0 + ser;
                    direct_cost = cost; direct_idx = (int)ci;
                } else {
                    int pl = cd.plane;
                    simtime_picosec t0 = now_ps;
                    if (ocs_up_free[pl][phys_src] > t0) t0 = ocs_up_free[pl][phys_src];
                    if (ocs_down_free[pl][phys_dst] > t0) t0 = ocs_down_free[pl][phys_dst];
                    reuse = (ocs_up_peer[pl][phys_src] == (int)phys_dst &&
                             ocs_down_peer[pl][phys_dst] == (int)phys_src);
                    tstart = t0 + (reuse ? 0 : ocs_reconf);
                    double Bpns = (double)cd.hop_queues[0]->link_bitrate() / 8.0 / 1e9;
                    if (panel_policy == PanelPolicy::Static) {
                        // static: unloaded costs -- no wait awareness; a plane
                        // is priced at T_r + path latency + serialization only.
                        cost = timeAsNs(ocs_reconf)
                             + (double)cd.latency_sum / 1000.0
                             + (double)msg_size / Bpns;
                    } else {
                        cost = timeAsNs(tstart - now_ps)
                             + (double)cd.latency_sum / 1000.0
                             + (double)msg_size / Bpns;
                    }
                }
                if (best < 0 || cost < best_cost) {
                    best = (int)ci; best_cost = cost; best_reuse = reuse; best_start = tstart;
                }
            }
            if (panel_policy == PanelPolicy::DirectPref && direct_idx >= 0 &&
                best != direct_idx &&
                direct_cost <= direct_preference_factor * best_cost) {
                best = direct_idx;
            }
            panel_choice = &(*panel_cands)[best];
            if (panel_choice->is_plane) {
                // ocs lease commit
                int pl = panel_choice->plane;
                double Bpns = (double)panel_choice->hop_queues[0]->link_bitrate() / 8.0 / 1e9;
                simtime_picosec occ = (simtime_picosec)(((double)msg_size / Bpns) * 1000.0)
                                      + timeFromNs(3.0 * 1500.0 / Bpns);
                simtime_picosec rel = best_start + occ;
                ocs_up_free[pl][phys_src] = rel;
                ocs_down_free[pl][phys_dst] = rel;
                ocs_up_peer[pl][phys_src] = (int)phys_dst;
                ocs_down_peer[pl][phys_dst] = (int)phys_src;
                if (best_reuse) ocs_reuses++; else ocs_reconfigs++;
                ocs_wait_total += (best_start - eventlist.now());
                panel_flow_delay = best_start - eventlist.now();
            } else {
                for (size_t k = 0; k < panel_choice->hop_queues.size(); k++)
                    panel_choice->hop_queues[k]->reserve_bytes(msg_size);
            }
        }
        } else {
            panel_choice = panel_route_select(
                panel_cands, msg_size, phys_src, phys_dst, panel_top->planes(),
                panel_policy != PanelPolicy::Static,
                panel_policy == PanelPolicy::DirectPref, direct_preference_factor);
            for (size_t k = 0; k < panel_choice->hop_queues.size(); k++)
                panel_choice->hop_queues[k]->reserve_bytes(msg_size);
        }
        auto key = std::make_pair(panel_choice->is_plane ? 1 : 0, panel_choice->hops);
        route_telemetry[key].first += 1;
        route_telemetry[key].second += msg_size;
    } else if (!net_paths[src][dst]) {
        net_paths[src][dst] = top->get_paths(src,dst);
    }


    if (algo == COUPLED_EPSILON) {
        mtcp = new MultipathTcpSrc(algo, eventlist, NULL, epsilon, false);
    }
    else {
        mtcp = new MultipathTcpSrc(algo, eventlist, NULL, 1000, false);
    }

    uint32_t it_sub;
    size_t crt_subflow_count = subflow_count;
    tot_subs += crt_subflow_count;
    cnt_con++;

    it_sub = panel_top ? 1
        : (crt_subflow_count > net_paths[src][dst]->size()?net_paths[src][dst]->size():crt_subflow_count);

#ifdef MH_FAT_TREE
    int use_all = it_sub==net_paths[src][dst]->size();
#endif

    for (uint32_t inter = 0; inter < it_sub; inter++) {
        tcpSrc = new TcpSrc(NULL, NULL, eventlist);
        tcpSrc->_debug_srcid = src;
        tcpSrc->_debug_dstid = dst;
        tcpSrc->astrasim_flow_finish_send_cb = (flow_id >= 900000000)
            ? &HTSimProtoTcp::stripe_finish_send : &HTSimSession::flow_finish_send;
        tcpSrc->set_flowsize(msg_size);
        if (nocc) {
            // Full window from the first RTT: no slow start, and with no drops
            // the AIMD path never executes. mss headroom covers rounding.
            uint64_t win = (uint64_t)msg_size + 2 * Packet::data_packet_size();
            if (nocc_maxwin > 0 && win > nocc_maxwin) {
                win = nocc_maxwin;
            }
            tcpSrc->set_cwnd(win);
            tcpSrc->set_ssthresh(win);
        }
        tcpSnk = new TcpSink();
        tcpSnk->_debug_srcid = src;
        tcpSnk->_debug_dstid = dst;
        tcpSnk->astrasim_flow_finish_recv_cb = (flow_id >= 900000000)
            ? &HTSimProtoTcp::stripe_finish_recv : &HTSimSession::flow_finish_recv;

        tcpSrc->setName("mtcp_" + ntoa(src) + "_" + ntoa(inter) + "_" + ntoa(dst));
        logfile->writeName(*tcpSrc);

        tcpSnk->setName("mtcp_sink_" + ntoa(src) + "_" + ntoa(inter) + "_" + ntoa(dst));
        logfile->writeName(*tcpSnk);

        tcpRtxScanner->registerTcp(*tcpSrc);
        size_t choice = 0;

#ifdef FAT_TREE
        if (!panel_top) {  // panel guard: choice made by policy above
            choice = rand()%net_paths[src][dst]->size();
        }
#endif

#ifdef OV_FAT_TREE
        choice = rand()%net_paths[src][dst]->size();
#endif

#ifdef MH_FAT_TREE
        if (use_all)
            choice = inter;
        else
            choice = rand()%net_paths[src][dst]->size();
#endif

#ifdef VL2
        choice = rand()%net_paths[src][dst]->size();
#endif

#ifdef STAR
        choice = 0;
#endif

#ifdef BCUBE
        //choice = inter;

        int min = -1, max = -1,minDist = 1000,maxDist = 0;
        if (subflow_count==1){
            //find shortest and longest path
            for (uint32_t dd=0;dd<net_paths[src][dst]->size();dd++){
                if (net_paths[src][dst]->at(dd)->size()<minDist){
                    minDist = net_paths[src][dst]->at(dd)->size();
                    min = dd;
                }
                if (net_paths[src][dst]->at(dd)->size()>maxDist){
                    maxDist = net_paths[src][dst]->at(dd)->size();
                    max = dd;
                }
            }
            choice = min;
        } else
            choice = rand()%net_paths[src][dst]->size();
#endif
        if (!panel_top && choice>=net_paths[src][dst]->size()){
            printf("Weird path choice %lu out of %lu\n",choice,net_paths[src][dst]->size());
            exit(1);
        }

#if PRINT_PATHS
        paths << "Route from "<< ntoa(src) << " to " << ntoa(dst) << "  (" << choice << ") -> " ;
        print_path(paths,net_paths[src][dst]->at(choice));
#endif

        if (panel_top) {
            routeout = new Route(*(panel_choice->route));
        } else {
            routeout = new Route(*(net_paths[src][dst]->at(choice)));
        }
        routeout->push_back(tcpSnk);

        routein = new Route();
        routein->push_back(tcpSrc);
        extrastarttime = 0 * drand();

        //join multipath connection

        mtcp->addSubflow(tcpSrc);

        if (inter == 0) {
            mtcp->setName("multipath" + ntoa(src) + "_" + ntoa(dst));
            logfile->writeName(*mtcp);
        }

        {
            simtime_picosec fd = 0;
            if (panel_top) { fd = panel_flow_delay; }
            tcpSrc->connect(*routeout, *routein, *tcpSnk, start + fd + timeFromMs(extrastarttime));
        }

        if (flow_id) {
            tcpSrc->setFlowId(flow_id);
            tcpSnk->setFlowId(flow_id);
        }

#ifdef PACKET_SCATTER
        tcpSrc->set_paths(net_paths[src][dst]);
        cout << "Using PACKET SCATTER!!!!"<<endl;
#endif

        if (ff&&!inter)
            ff->add_flow(src,dst,tcpSrc);

        sinkLogger->monitorMultipathSink(tcpSnk);
    }
    // panel candidate cleanup: candidate Route objects were copied into
    // routeout; free the originals and the vector.
    if (panel_cands) {
        for (size_t k = 0; k < panel_cands->size(); k++)
            delete (*panel_cands)[k].route;
        delete panel_cands;
    }
}

void HTSimProtoTcp::run(const HTSim::tm_info* const tm) {
    Logged::dump_idmap();
    // Record the setup
    int pktsize = Packet::data_packet_size();
    logfile->write("# pktsize=" + ntoa(pktsize) + " bytes");
    logfile->write("# subflows=" + ntoa(subflow_count));
    logfile->write("# hostnicrate = " + ntoa(linkspeed/1000000) + " Mbps");
    logfile->write("# corelinkrate = " + ntoa(HOST_NIC*CORE_TO_HOST) + " pkt/sec");
    double rtt = timeAsSec(timeFromUs(RTT));
    logfile->write("# rtt =" + ntoa(rtt));

    // GO!
    while (eventlist.doNextEvent()) {
    }
}

void HTSimProtoTcp::finish() {
    for (std::map<std::pair<int,int>, std::pair<uint64_t,uint64_t>>::iterator it =
             route_telemetry.begin(); it != route_telemetry.end(); ++it) {
        std::cout << "NETWORK_ROUTE class=" << (it->first.first ? "switch" : "direct")
                  << " hops=" << it->first.second
                  << " messages=" << it->second.first
                  << " payload_bytes=" << it->second.second
                  << " byte_hops=" << it->second.second * (uint64_t)it->first.second
                  << " propagation_ns=0 serialization_ns=0" << std::endl;
    }
    if (ocs_plan_mode) {
        for (std::map<std::pair<int,int>, std::pair<double,double>>::iterator
                 ci = ocs_cfg_times.begin(); ci != ocs_cfg_times.end(); ++ci) {
            int pl = ci->first.first, cf = ci->first.second;
            int ph = (cf >= 0 && cf < (int)ocs_cfgs[pl].size())
                         ? ocs_cfgs[pl][cf].phase : 0;
            std::cout << "OCS_CFG plane=" << pl << " cfg=" << cf
                      << " phase=" << ph
                      << " install_ns=" << ci->second.first
                      << " drain_ns=" << ci->second.second << std::endl;
        }
        std::cout << "OCS_RED red_GBps=" << (ocs_red_Bps / 1e9)
                  << " unfold_wait_ns=" << ocs_red_wait_ns << std::endl;
        std::cout << "OCS_PLAN_REPLAY reconfigurations=" << ocs_reconfigs
                  << " rounds_advanced=" << ocs_plan_rounds_done
                  << " scheduled_bytes=" << ocs_plan_scheduled
                  << " transmitted_bytes=" << ocs_plan_transmitted
                  << " reconf_ns=" << ocs_plan_reconf_ns << std::endl;
    }
    if (ocs_mode) {
        std::cout << "OCS_STATS reconfigs=" << ocs_reconfigs
                  << " circuit_reuses=" << ocs_reuses
                  << " total_wait_ns=" << timeAsNs(ocs_wait_total)
                  << " reconf_ns=" << timeAsNs(ocs_reconf) << std::endl;
    }
    std::cout << "Duplicate flow finishes ignored: "
              << HTSimSession::duplicate_finish_count << std::endl;
    std::cout << "Total TCP retransmissions: " << TcpSrc::_global_rtx_count
              << std::endl;
    if (nocc && TcpSrc::_global_rtx_count > 0) {
        std::cerr << "ERROR: " << TcpSrc::_global_rtx_count
                  << " retransmissions occurred under -nocc; results are not "
                     "bandwidth-determined. Increase -q (switch queue depth) "
                     "until this is zero." << std::endl;
        exit(2);
    }
    if (ocs_plan_mode) {
        const bool complete =
            ocs_expected_flows == ocs_started_flows &&
            ocs_expected_flows == ocs_completed_flows &&
            ocs_expected_stripes == ocs_started_stripes &&
            ocs_expected_stripes == ocs_completed_stripes &&
            ocs_expected_stripes == ocs_consumed_slots &&
            ocs_identity.assignments.empty() &&
            ocs_identity.stripe_slots.empty() &&
            ocs_runtime_flow_uid.empty() &&
            ocs_runtime_stripe_uid.empty() &&
            stripe_masters.empty() &&
            stripe_sub2master.empty() &&
            ocs_plan_transmitted == ocs_plan_scheduled &&
            ocs_fallback_lookups == 0 &&
            ocs_drained_configurations == ocs_expected_configurations &&
            ocs_config_drain_records == ocs_expected_configurations &&
            ocs_estimated_drain_events == 0 &&
            ocs_premature_advances == 0;
        if (!complete) ocs_plan_fatal("unconsumed_or_incomplete_plan_entries");
        ocs_print_plan_audit("PASS");
    }
#if USE_FIRST_FIT
    delete ff
#endif
    for (uint32_t i=0;i<no_of_nodes;i++){
        delete net_paths[i];
    }
    delete[] net_paths;
    delete[] is_dest;
    std::cout << std::endl << "Simulation of events finished" << std::endl;
}

} // namespace HTSim
