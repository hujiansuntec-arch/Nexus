# LibRpc Makefile

CXX = g++
AR = ar

# Compiler flags
CXXFLAGS = -std=c++14 -Wall -Wextra -O2 -g
CXXFLAGS += -Iinclude
CXXFLAGS += -pthread

# Linker flags
LDFLAGS = -pthread -lrt

# Directories
SRC_DIR = src
INC_DIR = include
BUILD_DIR = build
LIB_DIR = lib

# Output
LIBRARY = $(LIB_DIR)/librpc.a
SHARED_LIB = $(LIB_DIR)/librpc.so

# Test programs
TEST_INPROCESS = test_inprocess
TEST_DUPLEX = test_duplex_v2
TEST_LARGE_SENDER = test_large_sender
TEST_LARGE_RECEIVER = test_large_receiver

# Source files (updated to use V3 + LargeDataChannel)
SRCS = $(SRC_DIR)/NodeImpl.cpp \
       $(SRC_DIR)/UdpTransport.cpp \
       $(SRC_DIR)/SharedMemoryRegistry.cpp \
       $(SRC_DIR)/SharedMemoryTransportV3.cpp \
       $(SRC_DIR)/LargeDataChannel.cpp

# V2 source files (legacy - for reference)
SRCS_V2 = $(SRC_DIR)/SharedMemoryTransportV2.cpp

# V3 source files (dynamic shared memory)
SRCS_V3 = $(SRC_DIR)/SharedMemoryRegistry.cpp \
          $(SRC_DIR)/SharedMemoryTransportV3.cpp

OBJECTS = $(BUILD_DIR)/NodeImpl.o \
          $(BUILD_DIR)/UdpTransport.o \
          $(BUILD_DIR)/SharedMemoryRegistry.o \
          $(BUILD_DIR)/SharedMemoryTransportV3.o \
          $(BUILD_DIR)/LargeDataChannel.o

OBJECTS_V3 = $(BUILD_DIR)/SharedMemoryRegistry.o \
             $(BUILD_DIR)/SharedMemoryTransportV3.o

# Targets
.PHONY: all clean lib tests help run-tests run-duplex-test test-large

all: lib tests

lib: $(LIBRARY) $(SHARED_LIB)

tests: $(TEST_INPROCESS) $(TEST_DUPLEX)

test-large: $(TEST_LARGE_SENDER) $(TEST_LARGE_RECEIVER)

run-tests:
	@echo "Running in-process test..."
	./test_inprocess all

run-duplex-test:
	@echo "Running duplex test..."
	./run_duplex_test.sh

# Create directories
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(LIB_DIR):
	mkdir -p $(LIB_DIR)

# Static library
$(LIBRARY): $(OBJECTS) | $(LIB_DIR)
	@echo "Creating static library: $@"
	$(AR) rcs $@ $(OBJECTS)

# Shared library
$(SHARED_LIB): $(OBJECTS) | $(LIB_DIR)
	@echo "Creating shared library: $@"
	$(CXX) -shared -o $@ $(OBJECTS) $(LDFLAGS)

# Object files
$(BUILD_DIR)/NodeImpl.o: $(SRC_DIR)/NodeImpl.cpp | $(BUILD_DIR)
	@echo "Compiling: $<"
	$(CXX) $(CXXFLAGS) -fPIC -c $< -o $@

$(BUILD_DIR)/UdpTransport.o: $(SRC_DIR)/UdpTransport.cpp | $(BUILD_DIR)
	@echo "Compiling: $<"
	$(CXX) $(CXXFLAGS) -fPIC -c $< -o $@

$(BUILD_DIR)/SharedMemoryTransportV2.o: $(SRC_DIR)/SharedMemoryTransportV2.cpp | $(BUILD_DIR)
	@echo "Compiling: $<"
	$(CXX) $(CXXFLAGS) -fPIC -c $< -o $@

# V3 objects
$(BUILD_DIR)/SharedMemoryRegistry.o: $(SRC_DIR)/SharedMemoryRegistry.cpp | $(BUILD_DIR)
	@echo "Compiling: $<"
	$(CXX) $(CXXFLAGS) -fPIC -c $< -o $@

$(BUILD_DIR)/SharedMemoryTransportV3.o: $(SRC_DIR)/SharedMemoryTransportV3.cpp | $(BUILD_DIR)
	@echo "Compiling: $<"
	$(CXX) $(CXXFLAGS) -fPIC -c $< -o $@

$(BUILD_DIR)/LargeDataChannel.o: $(SRC_DIR)/LargeDataChannel.cpp | $(BUILD_DIR)
	@echo "Compiling: $<"
	$(CXX) $(CXXFLAGS) -fPIC -c $< -o $@

# Test programs
$(TEST_INPROCESS): test_inprocess.cpp $(LIBRARY)
	@echo "Building test: $@"
	$(CXX) $(CXXFLAGS) $< -o $@ -L$(LIB_DIR) -lrpc $(LDFLAGS)

$(TEST_DUPLEX): test_duplex_v2.cpp $(LIBRARY)
	@echo "Building duplex test: $@"
	$(CXX) $(CXXFLAGS) $< -o $@ -L$(LIB_DIR) -lrpc $(LDFLAGS)

# Large data channel tests
$(TEST_LARGE_SENDER): test_large_sender.cpp $(LIBRARY)
	@echo "Building large data sender: $@"
	$(CXX) $(CXXFLAGS) $< -o $@ -L$(LIB_DIR) -lrpc $(LDFLAGS)

$(TEST_LARGE_RECEIVER): test_large_receiver.cpp $(LIBRARY)
	@echo "Building large data receiver: $@"
	$(CXX) $(CXXFLAGS) $< -o $@ -L$(LIB_DIR) -lrpc $(LDFLAGS)

# Clean
clean:
	@echo "Cleaning..."
	rm -rf $(BUILD_DIR) $(LIB_DIR)
	rm -f $(TEST_INPROCESS) $(TEST_DUPLEX) $(TEST_LARGE_SENDER) $(TEST_LARGE_RECEIVER)
	rm -f /dev/shm/librpc_shm_v2 /dev/shm/librpc_registry /dev/shm/librpc_node_* /dev/shm/test_channel
	@echo "Clean complete"

# Help
help:
	@echo "LibRPC Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all             - Build library and all tests"
	@echo "  lib             - Build static and shared libraries"
	@echo "  tests           - Build standard test programs"
	@echo "  test-large      - Build large data channel tests"
	@echo "  run-tests       - Run in-process test"
	@echo "  run-duplex-test - Run duplex communication test"
	@echo "  clean           - Remove build artifacts and shared memory"
	@echo "  help            - Show this help message"
	@echo ""
	@echo "Test Programs:"
	@echo "  test_inprocess        - In-process multi-node tests"
	@echo "  test_duplex_v2        - Full-duplex communication test"
	@echo "  test_large_sender     - Large data channel sender (high-freq)"
	@echo "  test_large_receiver   - Large data channel receiver (high-freq)"
	@echo ""
	@echo "Usage:"
	@echo "  make                  # Build library and standard tests"
	@echo "  make test-large       # Build large data tests"
	@echo "  make run-tests        # Run in-process test"
	@echo "  make run-duplex-test  # Run duplex test"
	@echo "  make clean            # Clean build"
	@echo ""
	@echo "Large Data Test:"
	@echo "  # Terminal 1:"
	@echo "  ./test_large_receiver"
	@echo "  # Terminal 2:"
	@echo "  ./test_large_sender [count] [size_kb]"
	@echo "  # Example: ./test_large_sender 100 1024  # 100次, 每次1MB"
	@echo "  ./test_interprocess_sender 10000     # Send messages"
	@echo "  ./test_cleanup                       # Cleanup tests"
	@echo ""
	@echo "V3 Features (Dynamic Shared Memory):"
	@echo "  - Lock-free SPSC queues (no mutex contention)"
	@echo "  - Dynamic per-node memory allocation"
	@echo "  - Supports up to 256 nodes (vs V2's 8)"
	@echo "  - Memory efficient: 8MB for 2 nodes (vs V2's 132MB)"
	@echo "  - Automatic cleanup when nodes exit"
	@echo "  - Heartbeat-based zombie node detection (5s timeout)"
	@echo "  - Same performance as V2: ~500,000 msg/s"
