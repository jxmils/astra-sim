/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/system/PacketBundle.hh"

using namespace AstraSim;

PacketBundle::PacketBundle(Sys* sys,
                           BaseStream* stream,
                           std::list<MyPacket*> locked_packets,
                           bool needs_processing,
                           bool send_back,
                           uint64_t size,
                           MemBus::Transmition transmition) {
    this->sys = sys;
    this->locked_packets = locked_packets;
    this->needs_processing = needs_processing;
    this->send_back = send_back;
    this->size = size;
    this->stream = stream;
    this->transmition = transmition;
    creation_time = Sys::boostedTick();
}

PacketBundle::PacketBundle(Sys* sys,
                           BaseStream* stream,
                           bool needs_processing,
                           bool send_back,
                           uint64_t size,
                           MemBus::Transmition transmition) {
    this->sys = sys;
    this->needs_processing = needs_processing;
    this->send_back = send_back;
    this->size = size;
    this->stream = stream;
    this->transmition = transmition;
    creation_time = Sys::boostedTick();
}

void PacketBundle::send_to_MA() {
    sys->memBus->send_from_NPU_to_MA(transmition, size, needs_processing,
                                     send_back, this);
}

void PacketBundle::send_to_NPU() {
    sys->memBus->send_from_MA_to_NPU(transmition, size, needs_processing,
                                     send_back, this);
}

void PacketBundle::call(EventType event, CallData* data) {
    if (needs_processing == true) {
        needs_processing = false;
        // this->delay[ns], size[bytes] local_mem_bw[bytes/s]
        this->delay = static_cast<uint64_t>(static_cast<double>(size) /
                                            sys->local_mem_bw * 1e9)  // write
                      + static_cast<uint64_t>(static_cast<double>(size) /
                                              sys->local_mem_bw * 1e9)  // read
                      + static_cast<uint64_t>(static_cast<double>(size) /
                                              sys->local_mem_bw * 1e9);  // read
        sys->try_register_event(this, EventType::CommProcessingFinished, data,
                                this->delay);
        return;
    }
    // Memory-safety fix (timing-neutral). locked_packets holds raw pointers
    // into the owning algorithm's std::list<MyPacket> (Ring, HalvingDoubling).
    // That list's front is popped by reduce() whenever *any* bundle of the
    // stream completes, not necessarily this bundle's own packet, so when two
    // bundles complete out of insertion order (a processed packet followed by
    // a zero-latency one) the pointers held here are dangling and the former
    //     packet->ready_time = Sys::boostedTick();
    // was a write into freed memory (glibc: "corrupted size vs. prev_size";
    // ASan: heap-use-after-free, PacketBundle.cc <- Ring::reduce pop_front).
    // MyPacket::ready_time has no reader anywhere, so dropping the write
    // changes no event, no send, and no timestamp.
    stream->call(EventType::General, data);
    delete this;
}
