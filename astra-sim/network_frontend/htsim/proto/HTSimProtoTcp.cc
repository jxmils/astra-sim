#include "HTSimProtoTcp.hh"
#include "HTSimSession.hh"
#include "OcsPlanLoader.hh"
#include <climits>
#include <set>

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
    eventlist.setEndtime(timeFromSec(4));
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
    else { std::cerr << "Unknown -panel kind " << panel_kind << std::endl; exit(1); }
    panel_top = new PanelTopology(no_of_nodes, b, panel_planes,
                                  panel_link_gibps, panel_latency,
                                  panel_plane_gibps, panel_plane_latency,
                                  memFromPkt(queuesize_pkts), logfile.get(), &eventlist,
                                  panel_extents);
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


void HTSimProtoTcp::load_ocs_plan() {
    OcsPlanData plan;
    std::string err;
    if (!load_ocs_plan_file(ocs_plan_path, plan, err)) {
        std::cerr << "OCS plan load failed: " << err << std::endl; exit(1);
    }
    if (plan.planes != panel_planes) {
        std::cerr << "Plan planes " << plan.planes << " != topology planes "
                  << panel_planes << std::endl; exit(1);
    }
    ocs_plan_reconf_ns = plan.reconfiguration_ns;
    if (ocs_reconf != 10000) { ocs_plan_reconf_ns = timeAsNs(ocs_reconf); }
    ocs_initial_reconf = plan.initial_reconfiguration;
    ocs_plan_scheduled = plan.scheduled_bytes;
    ocs_cfgs.assign(plan.planes, {});
    ocs_cur.assign(plan.planes, 0);
    ocs_dark.assign(plan.planes, false);
    for (size_t k = 0; k < plan.configurations.size(); k++) {
        const OcsPlanData::Cfg& src = plan.configurations[k];
        OcsCfg oc; oc.round = src.round;
        oc.matching = src.matching;
        oc.force_reconf = src.force_reconf;
        int pl = src.plane;
        int cidx = (int)ocs_cfgs[pl].size();
        for (size_t q = 0; q < src.circuits.size(); q++) {
            int s = std::get<0>(src.circuits[q]);
            int d = std::get<1>(src.circuits[q]);
            uint64_t b = std::get<2>(src.circuits[q]);
            oc.circuits.push_back(std::make_tuple(s, d, b));
            ocs_slots[std::make_tuple(s, d, b, src.stream)].push_back({pl, cidx});
        }
        oc.remaining = (int)oc.circuits.size();
        ocs_cfgs[pl].push_back(oc);
    }
    for (size_t k = 0; k < plan.assignments.size(); k++) {
        ocs_route_kind[std::make_tuple(std::get<0>(plan.assignments[k]),
                                       std::get<1>(plan.assignments[k]),
                                       std::get<2>(plan.assignments[k]))]
            .push_back(std::get<3>(plan.assignments[k]) ? 'D' : 'O');
    }
    for (size_t k = 0; k < plan.assignments_full.size(); k++) {
        const OcsPlanData::Asn& a = plan.assignments_full[k];
        OcsAsn rec; rec.is_direct = a.is_direct; rec.stripes = a.stripes;
        ocs_assigns[std::make_tuple(a.src, a.dst, a.bytes, a.stream)]
            .push_back(rec);
    }
    s_self = this;
    std::cout << "OCS plan loaded: planes " << plan.planes
              << " rounds " << plan.rounds
              << " reconf_ns " << ocs_plan_reconf_ns
              << " scheduled_bytes " << ocs_plan_scheduled << std::endl;
}

void HTSimProtoTcp::ocs_install_next(int plane) {
    ocs_install_next_uncharged(plane, false);   // reconfig counted at drain time
}

void HTSimProtoTcp::ocs_install_next_uncharged(int plane, bool /*counted*/) {
    ocs_dark[plane] = false;
    ocs_cur[plane]++;
    ocs_plan_rounds_done++;
    auto key = std::make_pair(plane, ocs_cur[plane]);
    auto it = ocs_held.find(key);
    if (it != ocs_held.end()) {
        std::vector<std::pair<HTSim::FlowInfo,int>> flows;
        flows.swap(it->second);
        ocs_held.erase(it);
        ocs_releasing = true;
        ocs_releasing_plane = plane;
        for (size_t i = 0; i < flows.size(); i++) {
            schedule_htsim_event(flows[i].first, flows[i].second);
        }
        ocs_releasing = false;
        ocs_releasing_plane = -2;
    }
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
            sa.insert({std::get<0>(a.circuits[i]), std::get<1>(a.circuits[i])});
    if (!b.matching.empty())
        sb.insert(b.matching.begin(), b.matching.end());
    else
        for (size_t i = 0; i < b.circuits.size(); i++)
            sb.insert({std::get<0>(b.circuits[i]), std::get<1>(b.circuits[i])});
    return sa != sb;
}

static void ocs_drain_cb(void* arg) {
    std::pair<HTSimProtoTcp*, int>* pp = (std::pair<HTSimProtoTcp*, int>*)arg;
    pp->first->ocs_drain_reached(pp->second);
    delete pp;
}

// The installed configuration's circuits have all finished SERIALIZING
// (drained their uplinks). Mirroring the analytical OcsSwitch: the circuit can
// be torn down now -- in-flight propagation completes after removal. Charge
// T_r only if the next matching differs.
void HTSimProtoTcp::ocs_drain_reached(int plane) {
    int cfg = ocs_cur[plane];
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

void HTSimProtoTcp::flow_done(int flow_id) {
    ocs_flow_cfg.erase(flow_id);   // advancement is drain-driven (see ocs_drain_reached)
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

void HTSimProtoTcp::stripe_finish_recv(int, int, int, int tag) {
    HTSimProtoTcp* self = s_self;
    if (!self) return;
    auto mi = self->stripe_sub2master.find(tag);
    if (mi == self->stripe_sub2master.end()) return;
    int& st = stripe_sub_state[tag];
    if (st & 2) return;
    st |= 2;
    self->ocs_flow_cfg.erase(tag);
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
    if (it == ocs_flow_cfg.end()) return;
    int pl = it->second.first, cfgi = it->second.second;
    OcsCfg& oc = ocs_cfgs[pl][cfgi];
    oc.started++;
    simtime_picosec drain = eventlist.now()
        + (simtime_picosec)((double)bytes * 8.0 * 1e12 / (double)plane_rate)
        + (simtime_picosec)(2.0 * 1500.0 * 8.0 * 1e12 / (double)plane_rate);
    if (drain > oc.max_drain) oc.max_drain = drain;
    if (oc.started == (int)oc.circuits.size() && !oc.advance_scheduled &&
        ocs_cur[pl] == cfgi) {
        oc.advance_scheduled = true;
        long double delta_ns = timeAsNs(oc.max_drain - eventlist.now());
        if (delta_ns < 0) delta_ns = 0;
        std::pair<HTSimProtoTcp*, int>* arg = new std::pair<HTSimProtoTcp*, int>(this, pl);
        HTSimSession::instance().schedule_astra_event(delta_ns, &ocs_drain_cb, arg);
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
    // ocs_plan_mode dispatch: consult the plan; hold flows whose configuration
    // is not yet installed. (Uses logical->physical ids like everything else.)
    if (panel_top && ocs_plan_mode && ocs_releasing) {
        ocs_forced_plane = ocs_releasing_plane;
        ocs_plan_transmitted += flow.size;
    } else if (panel_top && ocs_plan_mode) {
        uint32_t ps = (panel_perm.empty() || (size_t)flow.src >= panel_perm.size())
            ? (uint32_t)flow.src : (uint32_t)panel_perm[flow.src];
        uint32_t pd = (panel_perm.empty() || (size_t)flow.dst >= panel_perm.size())
            ? (uint32_t)flow.dst : (uint32_t)panel_perm[flow.dst];
        // v5 assignment lookup: exact stream first, then any-stream fallback.
        OcsAsn* asn = NULL;
        {
            auto ai = ocs_assigns.find(std::make_tuple((int)ps, (int)pd,
                                        (uint64_t)flow.size, flow.tag));
            if (ai == ocs_assigns.end() || ai->second.empty()) {
                auto lo = ocs_assigns.lower_bound(std::make_tuple((int)ps, (int)pd,
                                                  (uint64_t)flow.size, INT_MIN));
                for (; lo != ocs_assigns.end(); ++lo) {
                    if (std::get<0>(lo->first) != (int)ps ||
                        std::get<1>(lo->first) != (int)pd ||
                        std::get<2>(lo->first) != (uint64_t)flow.size) break;
                    if (!lo->second.empty()) { ai = lo; break; }
                }
                if (lo == ocs_assigns.end() ||
                    std::get<0>(lo->first) != (int)ps) ai = ocs_assigns.end();
            }
            if (ai != ocs_assigns.end() && !ai->second.empty()) {
                static thread_local OcsAsn rec;
                rec = ai->second.front(); ai->second.pop_front();
                asn = &rec;
            }
        }
        if (asn && !asn->is_direct && asn->stripes.size() > 1) {
            // Striped optical transfer: split into per-plane sub-flows; ASTRA
            // sees completion when the last stripe drains.
            StripeMaster m; m.src = (int)src; m.dst = (int)dst;
            m.total = flow.size;
            m.pending_send = m.pending_recv = (int)asn->stripes.size();
            stripe_masters[flow_id] = m;
            for (size_t si = 0; si < asn->stripes.size(); si++) {
                int pl = asn->stripes[si].first;
                uint64_t sb = asn->stripes[si].second;
                int sub = stripe_next_tag++;
                stripe_sub2master[sub] = flow_id;
                HTSim::FlowInfo sf = flow; sf.size = sb; sf.tag = flow.tag;
                // slot lookup for this stripe (same key discipline as below)
                std::pair<int,int> slot(-1, -1);
                auto si2 = ocs_slots.find(std::make_tuple((int)ps, (int)pd,
                                                          sb, flow.tag));
                if (si2 != ocs_slots.end() && !si2->second.empty()) {
                    slot = si2->second.front(); si2->second.pop_front();
                } else {
                    auto lo = ocs_slots.lower_bound(std::make_tuple((int)ps, (int)pd,
                                                    sb, INT_MIN));
                    for (; lo != ocs_slots.end(); ++lo) {
                        if (std::get<0>(lo->first) != (int)ps ||
                            std::get<1>(lo->first) != (int)pd ||
                            std::get<2>(lo->first) != sb) break;
                        if (!lo->second.empty()) {
                            slot = lo->second.front(); lo->second.pop_front(); break;
                        }
                    }
                }
                if (slot.first < 0) {
                    std::cerr << "OCS plan has no stripe slot for " << ps << "->"
                              << pd << " bytes " << sb << std::endl;
                    exit(3);
                }
                if (slot.first != pl) {
                    std::cerr << "stripe plane mismatch: plan says " << pl
                              << " slot says " << slot.first << std::endl;
                }
                ocs_flow_cfg[sub] = slot;
                if (!(ocs_cur[slot.first] == slot.second && !ocs_dark[slot.first])) {
                    ocs_held[std::make_pair(slot.first, slot.second)]
                        .push_back({sf, sub});
                } else {
                    ocs_releasing = true; ocs_releasing_plane = slot.first;
                    schedule_htsim_event(sf, sub);
                    ocs_releasing = false; ocs_releasing_plane = -2;
                }
            }
            return;   // master flow is virtual; stripes carry the bytes
        }
        char kind = 'O';
        if (asn) {
            kind = asn->is_direct ? 'D' : 'O';
        } else {
            auto rk = ocs_route_kind.find(std::make_tuple((int)ps, (int)pd,
                                          (uint64_t)flow.size));
            if (rk != ocs_route_kind.end() && !rk->second.empty()) {
                kind = rk->second.front(); rk->second.pop_front();
            }
        }
        if (kind == 'D') {
            ocs_forced_plane = -1;
        } else {
            std::pair<int,int> slot(-1, -1);
            auto si = ocs_slots.find(std::make_tuple((int)ps, (int)pd,
                                                     (uint64_t)flow.size, flow.tag));
            if (si != ocs_slots.end() && !si->second.empty()) {
                slot = si->second.front(); si->second.pop_front();
            } else {
                // any-stream fallback: first non-empty deque for this (s,d,bytes)
                auto lo = ocs_slots.lower_bound(std::make_tuple((int)ps, (int)pd,
                                                (uint64_t)flow.size, INT_MIN));
                for (; lo != ocs_slots.end(); ++lo) {
                    if (std::get<0>(lo->first) != (int)ps ||
                        std::get<1>(lo->first) != (int)pd ||
                        std::get<2>(lo->first) != (uint64_t)flow.size) break;
                    if (!lo->second.empty()) {
                        slot = lo->second.front(); lo->second.pop_front(); break;
                    }
                }
            }
            if (slot.first < 0) {
                std::cerr << "OCS plan has no slot for flow " << ps << "->" << pd
                          << " bytes " << flow.size << " tag " << flow.tag << std::endl;
                exit(3);
            }
            int pl = slot.first, cfgi = slot.second;
            if (!(ocs_cur[pl] == cfgi && !ocs_dark[pl])) {
                if (cfgi < ocs_cur[pl]) {
                    std::cerr << "Warning: flow for past config plane " << pl
                              << " cfg " << cfgi << " (cur " << ocs_cur[pl] << ")" << std::endl;
                } else {
                    ocs_flow_cfg[flow_id] = slot;
                    ocs_held[std::make_pair(pl, cfgi)].push_back({flow, flow_id});
                    return;    // held until the configuration installs
                }
            }
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
        mtcp = new MultipathTcpSrc(algo, eventlist, NULL, epsilon);
    }
    else {
        mtcp = new MultipathTcpSrc(algo, eventlist, NULL);
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