#!/bin/sh
# Compila ed esegue sul PC i test della libreria dh_config (serve g++).
set -e
cd "$(dirname "$0")"
SRC=../../lib/dh_config/src
g++ -std=c++17 -Wall -Wextra -O1 -g -fsanitize=address,undefined -I$SRC test_config.cpp $SRC/DhIni.cpp $SRC/DhConfigStore.cpp -o /tmp/dh_config_test
/tmp/dh_config_test
