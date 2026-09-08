#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <map>
#include <vector>
#include <ios>
#include <iostream>
#include <fstream>
#include <set>
#include <string>

#include "HTSimSessionImpl.hh"
#include "OcsPlanLoader.hh"
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
        bool transport_backend_quiescent() const override;
        void wait_for_plan_round(
            int64_t round, EventHandler msg_handler, void* fun_arg) override;

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
        // Cap on the -nocc window (bytes). 0 = uncapped (cwnd = flow size).
        // A cap >= the per-flow bandwidth-delay product keeps FCT
        // bandwidth-determined while bounding in-flight bytes, which is what
        // makes multi-GiB collectives simulable: uncapped, in-flight ~ S and
        // bisection buffers would need gigabytes.
        uint64_t nocc_maxwin = 0;
        // --- Dynamic OCS mode: planes are circuit switches with leases ---
        // A flow using plane p holds its (uplink src, downlink dst) circuit
        // until sender-side final-ACK completion. Compatible flows may share
        // the standing circuit; an incompatible request waits until both
        // endpoint reference counts reach zero before reconfiguration.
        bool ocs_mode = false;
        simtime_picosec ocs_reconf = 10000;   // 10 ns default, in ps
        // Per plane, per node: installed peer, configuration-ready time, and
        // active transport references. No serialization estimate releases a
        // lease.
        std::vector<std::vector<int>> ocs_up_peer, ocs_down_peer;
        std::vector<std::vector<simtime_picosec>> ocs_up_ready, ocs_down_ready;
        std::vector<std::vector<uint32_t>> ocs_up_active, ocs_down_active;
        std::vector<bool> ocs_dynamic_plane_configured;
        struct DynamicLease {
            int plane;
            uint32_t src;
            uint32_t dst;
            simtime_picosec ready;
        };
        struct DynamicWaiter {
            HTSim::FlowInfo flow;
            int flow_id;
        };
        std::map<int, DynamicLease> ocs_dynamic_leases;
        std::deque<DynamicWaiter> ocs_dynamic_waiters;
        std::set<int> ocs_dynamic_queued;
        std::map<int, simtime_picosec> ocs_dynamic_requested_at;
        uint64_t ocs_dynamic_acquired = 0;
        uint64_t ocs_dynamic_completed = 0;
        uint64_t ocs_dynamic_queued_flows = 0;
        uint64_t ocs_dynamic_retry_attempts = 0;
        uint64_t ocs_dynamic_estimated_release_events = 0;
        uint64_t ocs_dynamic_premature_reconfigs = 0;
        uint64_t ocs_dynamic_initial_configurations = 0;
        void ocs_release_dynamic_lease(int flow_id);
        void ocs_retry_dynamic_waiters();
        uint64_t ocs_reconfigs = 0, ocs_reuses = 0;
        // --- Plan-driven OCS (merged model): executes the same ocs-plan.json
        // as the analytical OcsSwitch. Per plane: an ordered configuration
        // sequence (matchings with per-circuit byte quotas). A flow assigned
        // to configuration k of plane p starts only while that configuration
        // is installed; when a configuration's circuits all complete, the
        // plane goes dark for reconfiguration_ns and installs the next one.
        std::string ocs_plan_path;
        bool ocs_plan_mode = false;
        bool ocs_initial_reconf = false;
        double ocs_plan_reconf_ns = 0.0;
    public:  // OCS plan executor (callback needs access)
        struct OcsCircuit { int src; int dst; uint64_t bytes;
                            std::string flow_uid; std::string stripe_uid; };
        struct OcsCfg { int round; std::vector<OcsCircuit> circuits;
                        int remaining; int started = 0; int completed = 0;
                        simtime_picosec last_start = 0;
                        simtime_picosec first_complete = 0;
                        simtime_picosec last_complete = 0;
                        bool has_completion = false;
                        std::vector<std::pair<int,int>> matching;   // installed pairs
                        bool force_reconf = false;
                         int phase = 0;  // 1 fold, 2 unfold
                         bool synchronize = false;
                        bool drained = false;
                        bool drain_reported = false; };
        std::vector<std::vector<OcsCfg>> ocs_cfgs;      // [plane][seq]
        // Configuration zero is the first requested configuration, but it is
        // not installed until its cold-start delay has elapsed.
        std::vector<int> ocs_cur;
        std::vector<bool> ocs_dark;                      // reconfiguring
        struct OcsColdWaiter { HTSim::FlowInfo flow; int flow_id; };
        std::deque<OcsColdWaiter> ocs_cold_waiters;
        std::set<int> ocs_cold_queued_flow_ids;
        std::set<std::string> ocs_cold_queued_flow_uids;
        std::vector<bool> ocs_initial_requested;
        std::vector<bool> ocs_initial_active;
        std::vector<simtime_picosec> ocs_initial_request_time;
        std::vector<simtime_picosec> ocs_initial_ready_time;
        struct OcsPlanRoundWaiter {
            int64_t round;
            EventHandler msg_handler;
            void* fun_arg;
        };
        std::deque<OcsPlanRoundWaiter> ocs_plan_round_waiters;
        uint64_t ocs_plan_round_wait_requests = 0;
        uint64_t ocs_plan_round_wait_releases = 0;
        uint64_t ocs_expected_initial_configurations = 0;
        uint64_t ocs_initial_configuration_requests = 0;
        uint64_t ocs_initial_configuration_activations = 0;
        OcsPlanIdentityIndex ocs_identity;
        std::set<std::string> ocs_expected_flows;
        std::set<std::string> ocs_expected_stripes;
        std::set<std::string> ocs_started_flows;
        std::set<std::string> ocs_completed_flows;
        std::set<std::string> ocs_started_stripes;
        std::set<std::string> ocs_completed_stripes;
        std::set<std::string> ocs_consumed_slots;
        std::map<int, std::string> ocs_runtime_flow_uid;
        std::map<int, std::string> ocs_runtime_stripe_uid;
        std::vector<bool> ocs_retired_runtime_flow_ids;
        uint64_t ocs_runtime_flows_registered = 0;
        uint64_t ocs_runtime_flows_started = 0;
        uint64_t ocs_runtime_flows_completed = 0;
        uint64_t ocs_runtime_flows_retired = 0;
        uint64_t ocs_unknown_completions = 0;
        uint64_t ocs_duplicate_completions = 0;
        uint64_t ocs_fallback_lookups = 0;
        bool ocs_audit_printed = false;
        // striped-transfer master accounting: ASTRA sees one flow id; each
        // stripe is an internal sub-flow with a synthetic negative tag.
        struct StripeMaster { int src, dst; uint64_t total;
                              int pending_send, pending_recv;
                              bool sent_fwd = false, recv_fwd = false; };
        std::map<int, StripeMaster> stripe_masters;   // master tag -> state
        std::map<int, int> stripe_sub2master;         // sub tag -> master tag
        int stripe_next_tag = 900000000;   // below FLOW_ID_DYNAMIC_BASE, above ASTRA tags
        static HTSimProtoTcp* s_self;                 // for stripe callbacks
        static void stripe_finish_send(int src, int dst, int bytes, int tag);
        static void stripe_finish_recv(int src, int dst, int bytes, int tag);
        std::map<int, std::pair<int,int>> ocs_flow_cfg;  // flow_id -> (plane,cfg)
        uint64_t ocs_plan_scheduled = 0, ocs_plan_transmitted = 0;
        int ocs_plan_rounds_done = 0;
        void load_ocs_plan();
        void ocs_install_next(int plane);
        void ocs_install_next_uncharged(int plane, bool counted);
        void ocs_activate_initial_configuration(int plane);
        void ocs_request_initial_configuration(
            int plane, const char* request_source);
        void ocs_note_dynamic_initial_activation(
            int plane, simtime_picosec request_time,
            simtime_picosec ready_time);
        void ocs_retry_cold_waiters();
        bool ocs_plan_round_active(int64_t round);
        void ocs_retry_plan_round_waiters();
        // Per-configuration install/drain timestamps (ns), for causal-depth
        // analysis of tiled schedules.
        std::map<std::pair<int,int>, std::pair<double,double>> ocs_cfg_times;
        // Owner reduction service: busy-until per rank. Fold arrivals add
        // bytes/rate of service demand; unfold departures wait for it.
        std::vector<double> red_busy_until_ns;
        std::string panel_graphfile;       // -graph: Base::Custom edge list
        double ocs_red_Bps = 0.0;          // 0 = unmodelled (infinite)
        double ocs_red_wait_ns = 0.0;      // total unfold delay charged
        std::vector<char> red_gate_done;   // per plane: gate already applied
        std::map<int, std::pair<int,uint64_t>> red_flow_dst;
        std::map<int, int> red_flow_phase;
        bool matching_changed(const OcsCfg& a, const OcsCfg& b);
        void ocs_advance_after_drain(int plane);
        void ocs_drain_reached(int plane);
        void ocs_print_config_drain(int plane, int configuration,
                                    simtime_picosec advance_time);
        void ocs_note_started(int flow_id, uint64_t bytes, linkspeed_bps plane_rate);
        void ocs_note_stripe_completed(int flow_id);
        void ocs_print_plan_audit(const char* status);
        [[noreturn]] void ocs_plan_fatal(const std::string& reason);
        void ocs_validate_active_stripe(
            const OcsPlanData::Asn& assignment,
            const OcsPlanData::Stripe& stripe,
            const std::pair<int, int>& slot,
            uint32_t physical_source,
            uint32_t physical_destination);
        bool ocs_defer_for_initial_configuration(
            const HTSim::FlowInfo& flow, int flow_id);
        virtual void flow_done(int flow_id);
        simtime_picosec ocs_wait_total = 0;
        uint64_t ocs_expected_configurations = 0;
        uint64_t ocs_drained_configurations = 0;
        uint64_t ocs_config_drain_records = 0;
        uint64_t ocs_estimated_drain_events = 0;
        uint64_t ocs_premature_advances = 0;
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
        bool panel_plane_latency_explicit = false;
        int panel_planes = 0;
        std::string panel_kind;
        // Logical->physical rank permutation (collective embedding lever).
        // Empty = identity. "-permute qtp8" installs the Quartet-Then-Pair
        // 8-rank map: ASTRA's [4,2] dim-0 groups land on quartets {0,1,4,5},
        // {2,3,6,7} (complete K4s via ring D0 + planes P0,P1) and its dim-1
        // pairs land on the alternate ring matching D1.
        std::vector<int> panel_perm;
        std::vector<int> panel_extents;
        bool panel_nolog = false;   // suppress per-queue loggers ("-nolog")   // per-dim base extents ("-extents 4x8x8")
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
