/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

// TODO: HardwareResource.cc should be moved to the system layer.

#include "astra-sim/workload/HardwareResource.hh"
#include <cstdlib>
// ASTRA_CONCURRENT_SENDS=1: explicit COMM_SEND nodes do not take the per-NPU comm slot
static bool concurrent_sends() { static int v = -1; if (v < 0) { const char* e = getenv("ASTRA_CONCURRENT_SENDS"); v = (e && *e == '1') ? 1 : 0; } return v == 1; }
static bool exempt(const std::shared_ptr<Chakra::ETFeederNode>& node) {
    return node->type() == ChakraProtoMsg::COMM_RECV_NODE || (concurrent_sends() && node->type() == ChakraProtoMsg::COMM_SEND_NODE);
}

using namespace std;
using namespace AstraSim;
using namespace Chakra;

typedef ChakraProtoMsg::NodeType ChakraNodeType;

HardwareResource::HardwareResource(uint32_t num_npus)
    : num_npus(num_npus),
      num_in_flight_cpu_ops(0),
      num_in_flight_gpu_comm_ops(0),
      num_in_flight_gpu_comp_ops(0) {

    num_cpu_ops = 0;
    num_gpu_ops = 0;
    num_gpu_comms = 0;

    tics_cpu_ops = 0;
    tics_gpu_ops = 0;
    tics_gpu_comms = 0;

    cpu_ops_node = NULL;
    gpu_ops_node = NULL;
    gpu_comms_node = NULL;
}

void HardwareResource::occupy(const shared_ptr<Chakra::ETFeederNode> node) {
    if (node->is_cpu_op()) {
        assert(num_in_flight_cpu_ops == 0);
        ++num_in_flight_cpu_ops;
        ++num_cpu_ops;
    } else {
        if (node->type() == ChakraNodeType::COMP_NODE) {
            assert(num_in_flight_gpu_comp_ops == 0);
            ++num_in_flight_gpu_comp_ops;
            ++num_gpu_ops;
            gpu_ops_node = node;
        } else {
            if (exempt(node)) {
                return;
            }
            assert(num_in_flight_gpu_comm_ops == 0);
            ++num_in_flight_gpu_comm_ops;
            ++num_gpu_comms;
            gpu_comms_node = node;
        }
    }
}

void HardwareResource::release(const shared_ptr<Chakra::ETFeederNode> node) {
    if (node->is_cpu_op()) {
        --num_in_flight_cpu_ops;
        assert(num_in_flight_cpu_ops == 0);
    } else {
        if (node->type() == ChakraNodeType::COMP_NODE) {
            --num_in_flight_gpu_comp_ops;
            assert(num_in_flight_gpu_comp_ops == 0);
        } else {
            if (exempt(node)) {
                return;
            }
            --num_in_flight_gpu_comm_ops;
            assert(num_in_flight_gpu_comm_ops == 0);
        }
    }
}

bool HardwareResource::is_available(
    const shared_ptr<Chakra::ETFeederNode> node) const {
    if (node->is_cpu_op()) {
        if (num_in_flight_cpu_ops == 0) {
            return true;
        } else {
            return false;
        }
    } else {
        if (node->type() == ChakraNodeType::COMP_NODE) {
            if (num_in_flight_gpu_comp_ops == 0) {
                return true;
            } else {
                return false;
            }
        } else {
            if (num_in_flight_gpu_comm_ops == 0) {
                return true;
            } else {
                if (exempt(node)) {
                    return true;
                }
                if (num_in_flight_gpu_comm_ops == 0) {
                    return true;
                }
                return false;
            }
        }
    }
}

void HardwareResource::report() {
    cout << "num_cpu_ops: " << num_cpu_ops << endl;
    cout << "num_gpu_ops: " << num_gpu_ops << endl;
    cout << "num_gpu_comms: " << num_gpu_comms << endl;

    cout << "tics_cpu_ops: " << tics_cpu_ops << endl;
    cout << "tics_gpu_ops: " << tics_gpu_ops << endl;
    cout << "tics_gpu_comms: " << tics_gpu_comms << endl;
}
