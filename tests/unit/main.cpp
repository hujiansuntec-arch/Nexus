#include "simple_test.h"

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    return nexus_test::TestRegistry::instance().runAll();
}
