CXX      := clang++
CXXFLAGS := -std=c++17 -Wall -Wextra -g

TARGETS := TCP_server

all: $(TARGETS)

TCP_server: TCP_server.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -rf $(TARGETS) *.dSYM

.PHONY: all clean