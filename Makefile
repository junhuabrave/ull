CXX ?= g++
CPPFLAGS := -Iinclude
CXXFLAGS := -std=c++20 -O3 -g -Wall -Wextra -Wconversion -Werror -pthread

.PHONY: all test rtl-test telemetry-test sanitize clean
all: build/replay build/benchmark build/nasdaq_ingress

build:
	mkdir -p $@

build/%: src/%.cpp include/ull/engine.hpp include/ull/telemetry.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@

build/test_core: tests/test_core.cpp include/ull/engine.hpp include/ull/telemetry.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@

build/nasdaq_ingress: src/nasdaq_ingress.cpp include/ull/nasdaq/moldudp64.hpp include/ull/nasdaq/itch.hpp include/ull/nasdaq/itch_book.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@

build/test_moldudp64: tests/test_moldudp64.cpp include/ull/nasdaq/moldudp64.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@

build/test_itch: tests/test_itch.cpp include/ull/nasdaq/moldudp64.hpp include/ull/nasdaq/itch.hpp include/ull/nasdaq/itch_book.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@

build/test_order_entry: tests/test_order_entry.cpp include/ull/nasdaq/ouch.hpp include/ull/nasdaq/soupbintcp.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@

test: build/test_core build/test_moldudp64 build/test_itch build/test_order_entry
	./build/test_core
	./build/test_moldudp64
	./build/test_itch
	./build/test_order_entry

rtl-test: | build
	iverilog -g2012 -Wall -s quote_gate_tb -o build/quote_gate_tb rtl/quote_gate.v tests/quote_gate_tb.sv
	vvp build/quote_gate_tb

telemetry-test: build/replay
	.venv/bin/python -m unittest discover -s tests -p 'test_telemetry.py' -v

sanitize: | build
	$(CXX) $(CPPFLAGS) -std=c++20 -O1 -g -Wall -Wextra -pthread -fsanitize=address,undefined -fno-omit-frame-pointer tests/test_core.cpp -o build/test_sanitize
	./build/test_sanitize
	$(CXX) $(CPPFLAGS) -std=c++20 -O1 -g -Wall -Wextra -pthread -fsanitize=address,undefined -fno-omit-frame-pointer tests/test_moldudp64.cpp -o build/test_mold_sanitize
	./build/test_mold_sanitize
	$(CXX) $(CPPFLAGS) -std=c++20 -O1 -g -Wall -Wextra -pthread -fsanitize=address,undefined -fno-omit-frame-pointer tests/test_itch.cpp -o build/test_itch_sanitize
	./build/test_itch_sanitize
	$(CXX) $(CPPFLAGS) -std=c++20 -O1 -g -Wall -Wextra -pthread -fsanitize=address,undefined -fno-omit-frame-pointer tests/test_order_entry.cpp -o build/test_order_entry_sanitize
	./build/test_order_entry_sanitize

clean:
	rm -rf build
