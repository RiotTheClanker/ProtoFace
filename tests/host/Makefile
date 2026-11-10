# Host-side unit tests for the display-config code (no Pico needed).
# Fetch ArduinoJson first, e.g.:
#   git clone --depth 1 --branch v7.4.2 https://github.com/bblanchon/ArduinoJson tests/host/ArduinoJson
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined
INC = -IArduinoJson/src

test_config: test_config.cpp ../../src/pf_config.cpp ../../src/pf_config.h
	$(CXX) $(CXXFLAGS) $(INC) -o $@ test_config.cpp ../../src/pf_config.cpp

run: test_config
	./test_config

.PHONY: run
.DEFAULT_GOAL := run
