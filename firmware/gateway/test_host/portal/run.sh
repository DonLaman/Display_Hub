#!/bin/sh
# Prova sul PC delle parti pure del portale WiFi del gateway. Uso: ./run.sh
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -Wall -I. -I../../src test.cpp -o /tmp/dh_portal_test
/tmp/dh_portal_test
