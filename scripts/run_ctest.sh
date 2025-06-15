#!/bin/bash

# Usage:
# ./scripts/run_ctest.sh name_of_test_to_run

set -euxo pipefail

cd build && ninja
ctest -R $1 --verbose --timeout 30
