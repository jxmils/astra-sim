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
        virtual bool transport_backend_quiescent() const { return true; }
        virtual void schedule_htsim_event(FlowInfo flow, int flow_id) = 0;
        virtual void wait_for_plan_round(
            int64_t round, EventHandler msg_handler, void* fun_arg) = 0;

        void stop_simulation();
        void maybe_stop_after_transport_drain();

    private:
        bool application_complete = false;
        bool transport_drain_complete = false;
        simtime_picosec application_completion_time = 0;
};

} // namespace HTSim
