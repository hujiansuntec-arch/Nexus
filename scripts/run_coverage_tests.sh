#!/bin/bash

BUILD_DIR="build_test"
mkdir -p $BUILD_DIR
cd $BUILD_DIR

# Configure with coverage flags
cmake -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="--coverage" -DCMAKE_EXE_LINKER_FLAGS="--coverage" ..

# Build
make -j$(nproc)

# Clean previous coverage data
find . -name "*.gcda" -delete

echo "Running Unit Tests..."
./unit_tests || echo "Unit tests failed!"

echo "Running Integration Tests..."
./test_inprocess || echo "test_inprocess failed!"
./test_service_discovery || echo "test_service_discovery failed!"

# Cross-process tests
echo "Running Duplex Test..."
./test_duplex_v2 node0 node1 2 256 100 &
PID1=$!
./test_duplex_v2 node1 node0 2 256 100
wait $PID1 || echo "test_duplex_v2 failed!"

echo "Running Heartbeat Timeout Test..."
# One observer, one crasher
./test_heartbeat_timeout observer &
PID2=$!
sleep 1 # Wait for observer to start
./test_heartbeat_timeout crasher
wait $PID2 || echo "test_heartbeat_timeout failed!"

echo "Running Cross Process Discovery..."
./test_cross_process_discovery provider &
PID3=$!
sleep 1
./test_cross_process_discovery consumer
RET=$?
kill $PID3
wait $PID3 2>/dev/null
if [ $RET -ne 0 ]; then
    echo "test_cross_process_discovery failed!"
fi

echo "Running Cross Process Node Events..."
./test_cross_process_node_events observer &
PID4=$!
sleep 1
./test_cross_process_node_events joiner
RET=$?
kill $PID4
wait $PID4 2>/dev/null
if [ $RET -ne 0 ]; then
    echo "test_cross_process_node_events failed!"
fi

echo "All tests finished."
