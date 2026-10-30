CXX := clang++
CPPFLAGS := -Iinclude
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread
SOURCES := src/main.cpp src/engine.cpp src/storage.cpp

.PHONY: all test sanitize clean demo benchmark
all: build/lse

build/lse: $(SOURCES) include/lse/engine.hpp
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SOURCES) -o $@

build/unit_tests: tests/unit.cpp src/engine.cpp src/storage.cpp include/lse/engine.hpp
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/unit.cpp src/engine.cpp src/storage.cpp -o $@

test: build/lse build/unit_tests
	./build/unit_tests
	python3 tests/integration.py ./build/lse

sanitize:
	mkdir -p build
	$(CXX) $(CPPFLAGS) -std=c++17 -g -O1 -Wall -Wextra -Wpedantic -pthread -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit.cpp src/engine.cpp src/storage.cpp -o build/unit_sanitize
	./build/unit_sanitize
	$(CXX) $(CPPFLAGS) -std=c++17 -g -O1 -Wall -Wextra -Wpedantic -pthread -fsanitize=address,undefined -fno-omit-frame-pointer $(SOURCES) -o build/lse_sanitize
	python3 tests/integration.py ./build/lse_sanitize

demo: build/lse
	./build/lse index examples/corpus --index build/demo.lsi
	./build/lse search '"memory allocation" OR scheduler~1' --index build/demo.lsi --explain

build/benchmark_runner: tests/benchmark.cpp src/engine.cpp src/storage.cpp include/lse/engine.hpp
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/benchmark.cpp src/engine.cpp src/storage.cpp -o $@

benchmark: build/lse build/benchmark_runner
	python3 scripts/benchmark.py --binary ./build/lse --documents 3000 --output build/benchmark.json

clean:
	rm -rf build

PYTHON ?= .venv/bin/python
.PHONY: web-setup native web-demo api ui web web-test
web-setup:
	python3 -m venv .venv
	$(PYTHON) -m pip install -r requirements-web.txt
	$(PYTHON) scripts/build_native.py

native:
	$(PYTHON) scripts/build_native.py

web-demo: build/lse
	./build/lse index examples/corpus --index build/demo.lsi

api:
	$(PYTHON) -m uvicorn web.api:app --host 127.0.0.1 --port 8000

ui:
	$(PYTHON) -m streamlit run web/ui.py --server.address 127.0.0.1 --server.port 8501 --server.headless true

web: web-demo native
	$(PYTHON) scripts/run_web.py

web-test: build/lse
	$(PYTHON) -m pytest tests/test_web.py -q
