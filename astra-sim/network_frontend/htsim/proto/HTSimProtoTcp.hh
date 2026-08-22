#pragma once

#include <cstdint>
#include <memory>
#include <map>
#include <vector>
#include <ios>
#include <iostream>
#include <fstream>

#include "HTSimSessionImpl.hh"

#include "config.h"
#include "clock.h"
#include "mtcp.h"
#include "loggers.h"
#include "fat_tree_topology.h"
#include "panel_topology.h"

namespace HTSim {

class HTSimProtoTcp final : public HTSimSession::HTSimSessionImpl {
    public:
        HTSimProtoTcp(const HTSim::tm_info* const tm, int argc, char** argv);
        void run(const HTSim::tm_info* const tm);
        void stop_simulation();
        void finish();
        void send_flow(HTSim::FlowInfo flow,
                       int flow_id,
                       void (*msg_handler)(void* fun_arg),
                       void* fun_arg);
        void schedule_htsim_event(HTSim::FlowInfo flow, int flow_id);

    private:
        std::unique_ptr<Clock> c;
        linkspeed_bps linkspeed;
        static const uint32_t RTT = 10; // this is per link delay; identical RTT microseconds = 0.001 ms
        static const uint32_t DEFAULT_NODES = 16;
        int algo = COUPLED_EPSILON;
        double epsilon = 1;
        uint32_t no_of_conns = 0, no_of_nodes = DEFAULT_NODES;
        std::stringstream filename;
        uint32_t tot_subs = 0;
        uint32_t cnt_con = 0;

        TcpSrc* tcpSrc;
        TcpSink* tcpSnk;
        Route* routeout, *routein;
        double extrastarttime;
        MultipathTcpSrc* mtcp;
        std::map<uint32_t, std::vector<uint32_t>*>::iterator it;
        uint32_t connID = 0;

        std::unique_ptr<TcpSinkLoggerSampling> sinkLogger;
        std::unique_ptr<TcpRtxTimerScanner> tcpRtxScanner;
        std::unique_ptr<QueueLoggerFactory> qlf;
        std::unique_ptr<Logfile> logfile;

        vector<const Route*>*** net_paths;
        int* is_dest;

        char* topo_file = NULL;
        // Switch queue depth in packets. Settable via `-q <pkts>` in --htsim_opts.
        // Default 8 preserves historical behaviour; see BDP note in HTSimProtoTcp.cc.
        uint32_t queuesize_pkts = 8;
        // RNG seed for path choice. Settable via `-seed <int>`; defaults to
        // wall clock (old behaviour) but is always printed so runs can be replayed.
        unsigned rng_seed = 0;
        // `-nocc`: start every TcpSrc at full window (cwnd = ssthresh = flow
        // size), so there is no slow start and, absent drops, congestion
        // control never engages. FCT is then set by bandwidth, latency and
        // queueing only. Loss under -nocc is a fatal error at finish().
        bool nocc = false;
        // --- Panel mode: grid fabrics + switch planes with policy routing ---
        // Selected by `-panel <mesh2d|torus2d|mesh3d|torus3d|hybrid|fullswitch>`
        // in --htsim_opts. When null, behaviour is the stock fat-tree path.
        PanelTopology* panel_top = NULL;
        enum class PanelPolicy { Static, Adaptive, DirectPref };
        PanelPolicy panel_policy = PanelPolicy::Adaptive;
        double panel_link_gibps = 200.0;      // GiB/s, analytical convention
        double panel_plane_gibps = 200.0;
        simtime_picosec panel_latency = 0;    // set at parse (default 1000 ns)
        simtime_picosec panel_plane_latency = 0;
        int panel_planes = 0;
        std::string panel_kind;
        double direct_preference_factor = 1.10;
        // telemetry: (is_plane, hops) -> {messages, payload_bytes}
        std::map<std::pair<int,int>, std::pair<uint64_t,uint64_t>> route_telemetry;

        #ifdef FAT_TREE
        std::unique_ptr<FatTreeTopology> top;
        #endif

        #ifdef OV_FAT_TREE
        std::unique_ptr<OversubscribedFatTreeTopology> top;
        #endif

        #ifdef MH_FAT_TREE
        std::unique_ptr<MultihomedFatTreeTopology> top;
        #endif

        #ifdef STAR
        std::unique_ptr<StarTopology> top;
        #endif

        #ifdef BCUBE
        std::unique_ptr<BCubeTopology> top;
        #endif

        #ifdef VL2
        std::unique_ptr<VL2Topology> top;
        #endif

};

} // namespace HTSim