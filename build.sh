#!/bin/bash
# CMake build script for LibRPC

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
INSTALL_DIR="${SCRIPT_DIR}/install"

# Default values
BUILD_TYPE="Release"
PLATFORM="linux"
CLEAN=0
TESTS=ON
SHARED=ON

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Print usage
usage() {
    cat << EOF
Usage: $0 [OPTIONS]

Options:
    -h, --help          Show this help message
    -c, --clean         Clean build directory before building
    -d, --debug         Build in Debug mode (default: Release)
    -p, --platform      Target platform: linux|qnx (default: linux)
    -s, --static        Build static library only (default: shared)
    -t, --no-tests      Don't build tests
    --install-prefix    Installation prefix (default: ./install)

Examples:
    $0                          # Build for Linux (Release, shared lib)
    $0 -d                       # Build for Linux (Debug mode)
    $0 -p qnx                   # Cross-compile for QNX
    $0 -c -d -t                 # Clean build, Debug, no tests
    $0 --install-prefix=/usr/local  # Custom install path

EOF
    exit 0
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            usage
            ;;
        -c|--clean)
            CLEAN=1
            shift
            ;;
        -d|--debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        -p|--platform)
            PLATFORM="$2"
            shift 2
            ;;
        -s|--static)
            SHARED=OFF
            shift
            ;;
        -t|--no-tests)
            TESTS=OFF
            shift
            ;;
        --install-prefix)
            INSTALL_DIR="$2"
            shift 2
            ;;
        *)
            echo -e "${RED}Error: Unknown option $1${NC}"
            usage
            ;;
    esac
done

# Clean if requested
if [ $CLEAN -eq 1 ]; then
    echo -e "${YELLOW}Cleaning build directory...${NC}"
    rm -rf "${BUILD_DIR}"
fi

# Create build directory
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# Configure CMake
echo -e "${GREEN}=== Configuring LibRPC ===${NC}"
echo "Platform:       $PLATFORM"
echo "Build type:     $BUILD_TYPE"
echo "Shared library: $SHARED"
echo "Build tests:    $TESTS"
echo "Install prefix: $INSTALL_DIR"
echo ""

CMAKE_ARGS=(
    -DCMAKE_BUILD_TYPE=$BUILD_TYPE
    -DCMAKE_INSTALL_PREFIX=$INSTALL_DIR
    -DBUILD_SHARED_LIBS=$SHARED
    -DBUILD_TESTS=$TESTS
)

if [ "$PLATFORM" = "qnx" ]; then
    echo -e "${YELLOW}Configuring for QNX cross-compilation${NC}"
    if [ -z "$QNX_HOST" ] || [ -z "$QNX_TARGET" ]; then
        echo -e "${RED}Error: QNX environment not set!${NC}"
        echo "Please set QNX_HOST and QNX_TARGET environment variables"
        echo "Example:"
        echo "  export QNX_HOST=/opt/qnx710/host/linux/x86_64"
        echo "  export QNX_TARGET=/opt/qnx710/target/qnx7"
        exit 1
    fi
    CMAKE_ARGS+=(-DCMAKE_TOOLCHAIN_FILE=${SCRIPT_DIR}/cmake/qnx.cmake)
fi

cmake "${CMAKE_ARGS[@]}" ..

if [ $? -ne 0 ]; then
    echo -e "${RED}CMake configuration failed!${NC}"
    exit 1
fi

# Build
echo ""
echo -e "${GREEN}=== Building LibRPC ===${NC}"
cmake --build . -- -j$(nproc)

if [ $? -ne 0 ]; then
    echo -e "${RED}Build failed!${NC}"
    exit 1
fi

# Success
echo ""
echo -e "${GREEN}=== Build completed successfully ===${NC}"
echo "Build directory:   ${BUILD_DIR}"
echo "Library location:  ${BUILD_DIR}/librpc.*"

if [ "$TESTS" = "ON" ]; then
    echo "Test binaries:     ${BUILD_DIR}/test_*"
    echo ""
    echo "To run tests:"
    echo "  cd ${BUILD_DIR}"
    echo "  ./test_inprocess"
    echo "  ./test_duplex_v2"
fi

echo ""
echo "To install:"
echo "  cd ${BUILD_DIR}"
echo "  cmake --install ."
echo ""
echo "Or run: make install (if using Makefile generator)"
