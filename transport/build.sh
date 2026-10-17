#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
sdk_path="$(xcrun --sdk macosx --show-sdk-path)"
kernel_headers="$sdk_path/System/Library/Frameworks/Kernel.framework/Headers"
clang++ -arch x86_64 -std=c++14 -mkernel -fapple-kext \
  -fno-exceptions -fno-rtti -DKERNEL -DKERNEL_PRIVATE \
  -Wno-deprecated-declarations -isystem "$kernel_headers" \
  -c HostTransport.cpp -o HostTransport.o
clang++ -arch x86_64 -std=c++14 -mkernel -fapple-kext \
  -fno-exceptions -fno-rtti -DKERNEL -DKERNEL_PRIVATE \
  -Wno-deprecated-declarations -isystem "$kernel_headers" \
  -c HostSession.cpp -o HostSession.o
clang++ -std=c++14 -DRTW_TEST_HOST -fsanitize=address,undefined \
  -fno-omit-frame-pointer -g HostTransport.cpp tests/TransportTests.cpp \
  -o tests/transport-tests
./tests/transport-tests
clang++ -std=c++14 -DRTW_TEST_HOST -fsanitize=address,undefined \
  -fno-omit-frame-pointer -g HostSession.cpp HostTransport.cpp tests/SessionTests.cpp \
  -o tests/session-tests
./tests/session-tests
clang++ -std=c++14 -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  tests/PowerEventsTests.cpp -o tests/power-events-tests
./tests/power-events-tests
