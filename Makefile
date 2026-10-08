CXX      := clang++
CXXFLAGS := -std=c++17 -Wall -Wextra -g

TARGETS := udp_server

all: $(TARGETS)

udp_server: udp_server.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<