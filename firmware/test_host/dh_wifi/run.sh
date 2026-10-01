#!/bin/sh
# Compila ed esegue sul PC i test della scelta delle reti (serve g++).
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -Wall -Wextra -O1 -fsanitize=address,undefined -I../../lib/dh_wifi/src test_plan.cpp -o /tmp/dh_wifi_test
/tmp/dh_wifi_test
