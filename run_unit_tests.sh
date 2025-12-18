#!/bin/bash
set -e

cd /home/fz296w/workspace/Nexus/Nexus

# Clean build directory
rm -rf build_test
mkdir -p build_test
cd build_test

# Configure with coverage
cmake -DENABLE_COVERAGE=ON -DBUILD_TESTS=ON ..

# Build
make -j$(nproc) unit_tests

# Run tests
echo "Running Unit Tests..."
./unit_tests

# Generate coverage report if lcov is available
if command -v lcov >/dev/null 2>&1; then
    echo "Generating coverage report..."
    lcov --capture --directory . --output-file coverage.info
    lcov --remove coverage.info '/usr/*' '*/tests/*' --output-file coverage_filtered.info
    genhtml coverage_filtered.info --output-directory coverage_report
    echo "Coverage report generated in build_test/coverage_report/index.html"
else
    echo "lcov not found, skipping coverage report generation."
    echo "You can manually run gcov on the object files."
fi
