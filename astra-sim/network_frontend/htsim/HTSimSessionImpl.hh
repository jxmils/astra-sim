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

        void stop_simulation();
};

} // namespace HTSim