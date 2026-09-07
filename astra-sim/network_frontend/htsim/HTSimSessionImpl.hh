#pragma once

#include "HTSimSession.hh"
#include "eventlist.h"

namespace HTSim {

class HTSimSession::HTSimSessionImpl {
    public:
        EventList eventlist;

        // Pure abstract methods
        virtual void run(const HTSim::tm_info* const tm) = 0;
        virtual void finish() = 0;
        // Called when a flow fully completes (sender side); used by the
        // OCS plan executor to advance plane configurations.
        virtual void flow_done(int flow_id) {}
        virtual void schedule_htsim_event(FlowInfo flow, int flow_id) = 0;
        virtual void wait_for_plan_round(
            int64_t round, EventHandler msg_handler, void* fun_arg) = 0;

        void stop_simulation();
        void try_stop_simulation();
        void audit_transport_drain();

    protected:
        virtual bool transport_quiescent() const = 0;
        virtual void print_transport_drain_audit(
            const char* status,
            simtime_picosec application_completion_time,
            simtime_picosec drain_time) const = 0;

    private:
        bool application_complete_ = false;
        bool stop_requested_ = false;
        simtime_picosec application_completion_time_ = 0;
};

} // namespace HTSim
