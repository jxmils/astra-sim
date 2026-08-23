#include "HTSimProtoTcp.hh"
#include "HTSimSession.hh"

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
    else { std::cerr << "Unknown -panel kind " << panel_kind << std::endl; exit(1); }
    panel_top = new PanelTopology(no_of_nodes, b, panel_planes,
                                  panel_link_gibps, panel_latency,
                                  panel_plane_gibps, panel_plane_latency,
                                  memFromPkt(queuesize_pkts), logfile.get(), &eventlist);
    std::cout << "Panel topology: " << panel_kind << " nodes " << no_of_nodes
              << " planes " << panel_planes << " linkGiBps " << panel_link_gibps
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
    uint32_t phys_src = (panel_perm.empty() || src >= panel_perm.size())
        ? (uint32_t)src : (uint32_t)panel_perm[src];
    uint32_t phys_dst = (panel_perm.empty() || dst >= panel_perm.size())
        ? (uint32_t)dst : (uint32_t)panel_perm[dst];
    if (panel_top) {
        panel_cands = panel_top->get_candidates(phys_src, phys_dst);
        panel_choice = panel_route_select(
            panel_cands, msg_size, phys_src, phys_dst, panel_top->planes(),
            panel_policy != PanelPolicy::Static,
            panel_policy == PanelPolicy::DirectPref, direct_preference_factor);
        for (size_t k = 0; k < panel_choice->hop_queues.size(); k++)
            panel_choice->hop_queues[k]->reserve_bytes(msg_size);
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
        tcpSrc->astrasim_flow_finish_send_cb = &HTSimSession::flow_finish_send;
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
        tcpSnk->astrasim_flow_finish_recv_cb = &HTSimSession::flow_finish_recv;

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

        tcpSrc->connect(*routeout, *routein, *tcpSnk, start + timeFromMs(extrastarttime));

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