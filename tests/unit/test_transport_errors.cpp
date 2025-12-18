#include "simple_test.h"
#include "nexus/transport/SharedMemoryTransportV3.h"
#include "nexus/core/Config.h"

using namespace Nexus;
using namespace Nexus::rpc;

TEST(SharedMemoryTransportErrorTest, InvalidInit) {
    SharedMemoryTransportV3 transport;
    SharedMemoryTransportV3::Config config;
    
    // Empty Node ID
    ASSERT_FALSE(transport.initialize("", config));
    
    // Invalid Config (too many queues)
    // We need to know MAX_INBOUND_QUEUES. It's likely defined in SharedMemoryTransportV3.h or .cpp
    // Let's assume it's accessible or try a very large number.
    config.max_inbound_queues = 65535; 
    ASSERT_FALSE(transport.initialize("test_node", config));
}
