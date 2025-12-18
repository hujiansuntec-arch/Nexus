#!/bin/bash
set -e

# Build directory
BUILD_DIR="build"

# Create build directory if not exists
if [ ! -d "$BUILD_DIR" ]; then
    mkdir -p "$BUILD_DIR"
fi

cd "$BUILD_DIR"

# Configure with coverage
cmake -DENABLE_COVERAGE=ON ..

# Build
make -j$(nproc)

# Reset counters
lcov --directory . --zerocounters

# Run Unit Tests
rm -f /dev/shm/librpc_*
./unit_tests

# Run Integration Tests (Single Process)
rm -f /dev/shm/librpc_*

./test_service_cleanup
./test_inprocess 10 10

# Run Integration Tests (Multi Process)
echo "Running test_heartbeat_timeout..."
./test_heartbeat_timeout observer &
OBSERVER_PID=$!
sleep 1
./test_heartbeat_timeout crasher
wait $OBSERVER_PID

echo "Running test_cross_process_discovery..."
./test_cross_process_discovery provider &
PROVIDER_PID=$!
sleep 1
./test_cross_process_discovery consumer
kill $PROVIDER_PID
wait $PROVIDER_PID 2>/dev/null || true

echo "Running test_cross_process_node_events..."
./test_cross_process_node_events observer &
OBSERVER_PID=$!
sleep 1
./test_cross_process_node_events joiner
kill $OBSERVER_PID
wait $OBSERVER_PID 2>/dev/null || true

# Generate Report
lcov --directory . --capture --output-file coverage.info
lcov --remove coverage.info '/usr/*' '*/tests/*' '*/examples/*' '*/build/*' --output-file coverage_filtered.info
genhtml coverage_filtered.info --output-directory coverage_report

echo "Coverage report generated in build/coverage_report/index.html"

# Print summary
lcov --list coverage_filtered.info
