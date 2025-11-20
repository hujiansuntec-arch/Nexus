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
TEST_INTERPROCESS_RECEIVER = test_interprocess_receiver
TEST_INTERPROCESS_SENDER = test_interprocess_sender
TEST_CLEANUP = test_cleanup

# Source files (updated to use V2)
SRCS = $(SRC_DIR)/NodeImpl.cpp \
       $(SRC_DIR)/UdpTransport.cpp \
       $(SRC_DIR)/SharedMemoryTransportV2.cpp

OBJECTS = $(BUILD_DIR)/NodeImpl.o \
          $(BUILD_DIR)/UdpTransport.o \
          $(BUILD_DIR)/SharedMemoryTransportV2.o

# Targets
.PHONY: all clean lib tests help run-tests

all: lib tests

lib: $(LIBRARY) $(SHARED_LIB)

tests: $(TEST_INPROCESS) $(TEST_INTERPROCESS_RECEIVER) $(TEST_INTERPROCESS_SENDER) $(TEST_CLEANUP)

run-tests:
	@echo "Running test suite..."
	./run_tests.sh

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

# Test programs
$(TEST_INPROCESS): test_inprocess.cpp $(LIBRARY)
	@echo "Building test: $@"
	$(CXX) $(CXXFLAGS) $< -o $@ -L$(LIB_DIR) -lrpc $(LDFLAGS)

$(TEST_INTERPROCESS_RECEIVER): test_interprocess_receiver.cpp $(LIBRARY)
	@echo "Building test: $@"
	$(CXX) $(CXXFLAGS) $< -o $@ -L$(LIB_DIR) -lrpc $(LDFLAGS)

$(TEST_INTERPROCESS_SENDER): test_interprocess_sender.cpp $(LIBRARY)
	@echo "Building test: $@"
	$(CXX) $(CXXFLAGS) $< -o $@ -L$(LIB_DIR) -lrpc $(LDFLAGS)

$(TEST_CLEANUP): test_cleanup.cpp $(LIBRARY)
	@echo "Building test: $@"
	$(CXX) $(CXXFLAGS) $< -o $@ -L$(LIB_DIR) -lrpc $(LDFLAGS)

# Clean
clean:
	@echo "Cleaning..."
	rm -rf $(BUILD_DIR) $(LIB_DIR)
	rm -f $(TEST_INPROCESS) $(TEST_INTERPROCESS_RECEIVER) $(TEST_INTERPROCESS_SENDER) $(TEST_CLEANUP)
	rm -f /dev/shm/librpc_shm_v2
	@echo "Clean complete"

# Help
help:
	@echo "LibRPC Build System (Lock-Free Shared Memory V2)"
	@echo ""
	@echo "Targets:"
	@echo "  all        - Build library and all tests (default)"
	@echo "  lib        - Build static and shared libraries"
	@echo "  tests      - Build all test programs"
	@echo "  run-tests  - Run complete test suite (run_tests.sh)"
	@echo "  clean      - Remove build artifacts and shared memory"
	@echo "  help       - Show this help message"
	@echo ""
	@echo "Test Programs:"
	@echo "  test_inprocess               - In-process multi-node tests"
	@echo "  test_interprocess_receiver   - Inter-process receiver"
	@echo "  test_interprocess_sender     - Inter-process sender"
	@echo "  test_cleanup                 - Shared memory cleanup tests"
	@echo ""
	@echo "Usage:"
	@echo "  make              # Build library and tests"
	@echo "  make lib          # Build library only"
	@echo "  make tests        # Build test programs"
	@echo "  make run-tests    # Run all tests"
	@echo "  make clean        # Clean build"
	@echo ""
	@echo "Manual Test Run:"
	@echo "  ./test_inprocess all                 # In-process tests"
	@echo "  ./test_interprocess_receiver &       # Start receiver"
	@echo "  ./test_interprocess_sender 10000     # Send messages"
	@echo "  ./test_cleanup                       # Cleanup tests"
	@echo ""
	@echo "Features:"
	@echo "  - Lock-free SPSC queues (no mutex contention)"
	@echo "  - 132MB shared memory (8 nodes × 1024 msg/queue)"
	@echo "  - Automatic cleanup when last node exits"
	@echo "  - Heartbeat-based zombie node detection (5s timeout)"
	@echo "  - ~500,000 msg/s in-process throughput"
