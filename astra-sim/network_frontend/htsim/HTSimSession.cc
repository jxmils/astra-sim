#include "HTSimSession.hh"
#include "HTSimSessionImpl.hh"
#include "HTSimProtoTcp.hh"

#include <cstdlib>
#include <iostream>

namespace HTSim {

std::map<std::pair<int, HTSim::Dir>, int> HTSimSession::node_bytes_sent;
std::map<std::pair<HTSim::MsgEventKey, int>, HTSim::MsgEvent>
    HTSimSession::send_waiting;
std::map<HTSim::MsgEventKey, HTSim::MsgEvent> HTSimSession::recv_waiting;
std::map<HTSim::MsgEventKey, int> HTSimSession::msg_standby;
std::map<int, int> HTSimSession::flow_id_to_tag;
uint64_t HTSimSession::duplicate_finish_count = 0;
bool HTSimSession::quiet = false;

void HTSimSession::set_quiet(bool on) {
    quiet = on;
    TcpSrc::_quiet_flow_log = on;
}

void HTSimSession::set_recv_flow_finish(bool on) {
    conf.recv_flow_finish = on;
}

bool HTSimSession::reclaim = false;

void HTSimSession::set_reclaim(bool on) {
    reclaim = on;
}
HTSimSession* HTSimSession::session = nullptr;
HTSimConf HTSimSession::conf;

typedef void (*EventHandler)(void*);

class AstraEventSrc : public EventSource {
public:
    AstraEventSrc(EventHandler msg_handler, void* fun_arg, EventList& eventList);
    void doNextEvent();

    // Callbacks scheduled but not yet delivered. Serving mode reads this to
    // tell "the ASTRA side is waiting on simulated time" apart from "only
    // htsim's periodic samplers are pending".
    static uint64_t pending;
    // Serving mode: the source frees itself after delivering its callback
    // (every ASTRA callback used to leave one behind in astra_events).
    bool self_delete = false;

private:
    EventHandler _msg_handler;
    void* _fun_arg;
};

uint64_t AstraEventSrc::pending = 0;

// Event source for scheduling callbacks to be executed by HTSim
std::vector<AstraEventSrc*> astra_events;

AstraEventSrc::AstraEventSrc(EventHandler msg_handler,
                             void* fun_arg,
                             EventList& eventList)
    : EventSource(eventList, "astraSimSrc"), _msg_handler(msg_handler), _fun_arg(fun_arg) {
    ++pending;
}

void AstraEventSrc::doNextEvent() {
    --pending;
    // Run the handler
    _msg_handler(_fun_arg);
    if (self_delete)
        delete this;   // nothing references the source after delivery
}

std::stringstream& operator>> (std::stringstream& is, HTSimProto& proto) {
    std::string s;
    is >> s;
    if (s == "tcp") {
        proto = HTSimProto::Tcp;
    } else {
        proto = HTSimProto::None;
    }
    return is;
}

// Send_flow commands the HTSim simulator to schedule a message to be sent
// between two pair of nodes. send_flow is triggered by sim_send.
void HTSimSession::send_flow(FlowInfo flow,
                            int flow_id,
                            void (*msg_handler)(void* fun_arg),
                            void* fun_arg) {
    // Create a MsgEvent instance and register callback function.
    if (!quiet)
        std::cout << "Send flow " << flow_id << " from " << flow.src << " to " << flow.dst
                  << " with size " << flow.size << "\n";
    MsgEvent send_event = MsgEvent(flow.src, flow.dst, Dir::Send, flow.size, fun_arg, msg_handler);
    flow_id_to_tag[flow_id] = flow.tag;
    std::pair<MsgEventKey, int> send_event_key = std::make_pair(
        std::make_pair(flow.tag, std::make_pair(send_event.src_id, send_event.dst_id)), flow_id);
    HTSimSession::send_waiting[send_event_key] = send_event;

    // Create a queue pair and schedule within the HTSim simulator.
    impl->schedule_htsim_event(flow, flow_id);
}

// notify_receiver_receive_data looks at whether the astra-sim has issued
// sim_recv for this message. If the system layer is waiting for this message,
// call the callback handler for the MsgEvent. If the system layer is not *yet*
// waiting for this message, register that this message has arrived,
// so that the system layer can later call the callback handler when sim_recv
// is called.
void HTSimSession::notify_receiver_receive_data(int src_id,
                                                int dst_id,
                                                int message_size,
                                                int tag,
                                                int flow_id) {
    MsgEventKey recv_expect_event_key = make_pair(tag, make_pair(src_id, dst_id));

    if (HTSimSession::recv_waiting.find(recv_expect_event_key) !=
        HTSimSession::recv_waiting.end()) {
        // The Sys object is waiting for packets to arrive.
        MsgEvent recv_expect_event = recv_waiting[recv_expect_event_key];
        if (message_size == recv_expect_event.remaining_msg_bytes) {
            // We received exactly the amount of data what Sys object was expecting.
            HTSimSession::recv_waiting.erase(recv_expect_event_key);
            recv_expect_event.callHandler();
        } else if (message_size > recv_expect_event.remaining_msg_bytes) {
            // We received more packets than the Sys object is expecting.
            // Place task in received_msg_standby_hash and wait for Sys object to issue more sim_recv
            // calls. Call callback handler for the amount Sys object was waiting for.
            HTSimSession::msg_standby[recv_expect_event_key] = message_size - recv_expect_event.remaining_msg_bytes;
            HTSimSession::recv_waiting.erase(recv_expect_event_key);
            recv_expect_event.callHandler();
        } else {
            // There are still packets to arrive.
            // Reduce the number of packets we are waiting for. Do not call callback
            // handler.
            recv_expect_event.remaining_msg_bytes -= message_size;
            HTSimSession::recv_waiting[recv_expect_event_key] = recv_expect_event;
        }
    } else {
        // The Sys object is not yet waiting for packets to arrive.
        if (HTSimSession::msg_standby.find(recv_expect_event_key) ==
            HTSimSession::msg_standby.end()) {
            // Place task in msg_standby and wait for Sys object to issue more sim_recv
            // calls.
            HTSimSession::msg_standby[recv_expect_event_key] = message_size;
        } else {
            // Sys object is still waiting. Add number of bytes we are waiting for.
            HTSimSession::msg_standby[recv_expect_event_key] += message_size;
        }
    }

    // Add to the number of total bytes received.
    if (HTSimSession::node_bytes_sent.find(make_pair(dst_id, Dir::Receive)) ==
        HTSimSession::node_bytes_sent.end()) {
        HTSimSession::node_bytes_sent[make_pair(dst_id, Dir::Receive)] = message_size;
    } else {
        HTSimSession::node_bytes_sent[make_pair(dst_id, Dir::Receive)] += message_size;
    }
}

void HTSimSession::notify_sender_sending_finished(int src_id,
                                                  int dst_id,
                                                  int message_size,
                                                  int tag,
                                                  int flow_id) {
    // Lookup the send_event registered at send_flow().
    std::pair<MsgEventKey, int> send_event_key =
        make_pair(make_pair(tag, make_pair(src_id, dst_id)), flow_id);
    if (HTSimSession::send_waiting.find(send_event_key) == HTSimSession::send_waiting.end()) {
        // A retransmitted final packet can complete a flow twice; the first
        // completion erased the event. Only reachable under packet loss (a
        // -nocc run with adequate queues never retransmits), so tolerate it
        // with a count rather than aborting -- an abort here races the
        // loss-freedom check in HTSimProtoTcp::finish() and hides it.
        HTSimSession::duplicate_finish_count++;
        std::cerr << "Warning: duplicate flow-finish ignored (src " << src_id
                  << " dst " << dst_id << " tag " << tag << " flow " << flow_id
                  << ")\n";
        return;
    }

    // Verify that the (HTSim identified) sent message size matches what was
    // expected by the system layer.
    MsgEvent send_event = HTSimSession::send_waiting[send_event_key];
    HTSimSession::send_waiting.erase(send_event_key);

    // Add to the number of total bytes sent.
    if (HTSimSession::node_bytes_sent.find(make_pair(src_id, Dir::Send)) ==
        HTSimSession::node_bytes_sent.end()) {
        HTSimSession::node_bytes_sent[make_pair(src_id, Dir::Send)] = message_size;
    } else {
        HTSimSession::node_bytes_sent[make_pair(src_id, Dir::Send)] += message_size;
    }
    send_event.callHandler();
}

// flow_finish is triggered by HTSim to indicate that a flow has finished.
// Registered as the callback handler for the source
// instance created at send_flow.
void HTSimSession::flow_finish_send(int src_id, int dst_id, int msg_size, int flow_id) {

    int tag = flow_id_to_tag[flow_id];
    // Retire an exact planned OCS stripe or dynamic OCS lease before its ASTRA
    // completion handler can make dependent traffic runnable. Other backends'
    // flow_done implementations are no-ops.
    HTSimSession::instance().impl->flow_done(flow_id);
    // Let sender knows that the flow has finished.
    notify_sender_sending_finished(src_id, dst_id, msg_size, tag, flow_id);

    if (!conf.recv_flow_finish) {
        // Let receiver knows that it has received packets.
        notify_receiver_receive_data(src_id, dst_id, msg_size, tag, flow_id);
    }
    // Both directions have consumed the tag by now (the receive side fires
    // on the last data byte, before the sender's final ack).
    if (reclaim)
        flow_id_to_tag.erase(flow_id);
}

// flow_finish is triggered by HTSim to indicate that a flow has finished.
// Registered as the callback handler for the sink
// instance created at send_flow.
void HTSimSession::flow_finish_recv(int src_id, int dst_id, int msg_size, int flow_id) {

    if (conf.recv_flow_finish) {
        int tag = flow_id_to_tag[flow_id];
        // Let receiver knows that it has received packets.
        notify_receiver_receive_data(src_id, dst_id, msg_size, tag, flow_id);
    }

}

void HTSimSession::finish() {
    impl->finish();
}

void HTSimSession::stop_simulation() {
    impl->stop_simulation();
}

void HTSimSession::HTSimSessionImpl::stop_simulation() {
    std::cout << "HTSim stopping simulation..." << std::endl;
    auto now = eventlist.now();
    eventlist.setEndtime(now);
}

// Constructor creates inner impl
HTSimSession& HTSimSession::init(const HTSim::tm_info* const tm, const int argc, char** argv, const HTSimProto proto) {
    if (session != nullptr)
        assert(0 && "HTSim session initialized twice");
    static HTSimSession session_(tm, argc, argv, proto);
    session = &session_;
    return session_;
};

// Instance getter
HTSimSession& HTSimSession::instance() {
    if (session == nullptr)
        assert(0 && "HTSim session not initialized");
    return (*session);
}

HTSimSession::HTSimSession(const HTSim::tm_info* const tm, int argc, char** argv, HTSimProto proto) {
    switch(proto) {
        case HTSimProto::Tcp:
            impl = std::make_unique<HTSimProtoTcp>(tm, argc, argv);
            break;
        default:
            std::cerr << "Unknown HTSim protocol" << std::endl;
            abort();
    }
}

void HTSimSession::schedule_astra_event(long double when_ns,
                                        void (*msg_handler)(void* fun_arg),
                                        void* fun_arg) {
    if (!(when_ns >= 0)) {
        // A negative delta wraps simulated time (uint64 picoseconds) and
        // lands an event near 2^64; name it here rather than in the sampler
        // that trips over it a simulated second later.
        std::cerr << "HTSimSession::schedule_astra_event: negative delay "
                  << static_cast<double>(when_ns) << " ns at "
                  << get_time_ns() << " ns" << std::endl;
        abort();
    }
    AstraEventSrc* src = new AstraEventSrc(msg_handler, fun_arg, impl->eventlist);
    if (reclaim)
        src->self_delete = true;
    else
        astra_events.push_back(src);
    impl->eventlist.sourceIsPendingRel(*src, timeFromNs(when_ns));
}

void HTSimSession::wait_for_plan_round(
        int64_t round,
        void (*msg_handler)(void* fun_arg),
        void* fun_arg) {
    impl->wait_for_plan_round(round, msg_handler, fun_arg);
}

void HTSimSession::wait_for_plan_configuration(
        int plane,
        int configuration,
        void (*msg_handler)(void* fun_arg),
        void* fun_arg) {
    impl->wait_for_plan_configuration(
        plane, configuration, msg_handler, fun_arg);
}

// Wrapper functions

void HTSimSession::run(const HTSim::tm_info* const tm) {
    impl->run(tm);
}

double HTSimSession::get_time_ns() {
    return timeAsNs(impl->eventlist.now());
}

double HTSimSession::get_time_us() {
    return timeAsUs(impl->eventlist.now());
}

HTSimSession::~HTSimSession() {}

} // namespace HTSim

// Serving-mode stepping (HTSimMain --serving).

namespace HTSim {

namespace {
void noop_handler(void*) {}
}  // namespace

bool HTSimSession::step() {
    return impl->step();
}

void HTSimSession::advance_to_ns(double when_ns) {
    const simtime_picosec when = timeFromNs(when_ns);
    if (when <= impl->eventlist.now()) {
        return;
    }
    AstraEventSrc* src = new AstraEventSrc(&noop_handler, nullptr, impl->eventlist);
    if (reclaim)
        src->self_delete = true;
    else
        astra_events.push_back(src);
    EventList::sourceIsPending(*src, when);
}

} // namespace HTSim

namespace HTSim {

// True when nothing the frontend can observe is in flight: no ASTRA
// callback is scheduled (compute delays, deferred issues, the advance_to_ns
// no-op) and no flow is between sim_send and its completion. htsim's own
// periodic samplers and scanners do not count; they would otherwise keep
// the event list non-empty until the simulated end time.
bool HTSimSession::astra_idle() {
    return AstraEventSrc::pending == 0 && send_waiting.empty() && recv_waiting.empty();
}

uint64_t HTSimSession::pending_astra_events() {
    return AstraEventSrc::pending;
}

// Serving runs have no fixed simulated end: the frontend's "exit" ends them.
void HTSimSession::run_forever() {
    EventList::setEndtime(0);
}

} // namespace HTSim
