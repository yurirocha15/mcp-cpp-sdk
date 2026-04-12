.PHONY: all init init-dev init-docs build debug test clean format lint coverage docs

SUDO := $(shell [ $$(id -u 2>/dev/null || echo 1) -eq 0 ] || echo "sudo")
# Use half of the number of cores (cross-platform), ensuring a minimum of 1
NUM_CPU_2 := $(shell c=$$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2); h=$$((c / 2)); [ $$h -gt 0 ] && echo $$h || echo 1)

all: build

init:
	python3 scripts/init.py

init-dev:
	python3 scripts/init.py --dev

init-docs:
	python3 scripts/init.py --docs

build:
	conan install . --output-folder=build --build=missing -s compiler.cppstd=20 -c tools.cmake.cmaketoolchain:generator=Ninja
	@# Conan auto-includes all discovered preset files; strip extras to avoid duplicates.
	@echo '{"version":4,"vendor":{"conan":{}},"include":["build/CMakePresets.json"]}' > CMakeUserPresets.json
	cmake --preset conan-release
	cmake --build --preset conan-release -j$(NUM_CPU_2)

debug:
	conan install . --output-folder=build/debug --build=missing -s compiler.cppstd=20 -s build_type=Debug -c tools.cmake.cmaketoolchain:generator=Ninja
	cmake -B build/debug -DCMAKE_TOOLCHAIN_FILE=build/debug/conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -G Ninja
	cmake --build build/debug -j$(NUM_CPU_2)
	@# Conan adds build/debug to CMakeUserPresets.json, causing duplicate preset errors.
	@# Restore the file to only include the release preset.
	@echo '{"version":4,"vendor":{"conan":{}},"include":["build/CMakePresets.json"]}' > CMakeUserPresets.json

test:
	ctest --preset conan-release -j$(NUM_CPU_2)

coverage:
	conan install . --output-folder=build/coverage --build=missing -s compiler.cppstd=20 -s build_type=Debug -c tools.cmake.cmaketoolchain:generator=Ninja
	cmake --preset conan-debug -DENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug -B build/coverage
	cmake --build build/coverage -j$(NUM_CPU_2)
	ctest --test-dir build/coverage -j$(NUM_CPU_2) --output-on-failure
	gcovr -r . --html --html-details -o build/coverage/coverage.html -f include/
	gcovr -r . -f include/

docs:
	python3 scripts/init.py --docs
	conan install . --output-folder=build --build=missing -s compiler.cppstd=20 -c tools.cmake.cmaketoolchain:generator=Ninja
	@echo '{"version":4,"vendor":{"conan":{}},"include":["build/CMakePresets.json"]}' > CMakeUserPresets.json
	cmake --preset conan-release
	@if cmake --build --preset conan-release --target docs -j$(NUM_CPU_2) 2>/dev/null; then \
		echo "[+] Doxygen XML generated"; \
	else \
		echo "[!] Doxygen not available, building Sphinx docs without API reference"; \
	fi
	sphinx-build -b html docs build/docs/html

clean:
	rm -rf build

format:
	find include src test examples -name '*.hpp' -o -name '*.cpp' | xargs clang-format -i

lint:
	find include src test examples -name '*.hpp' -o -name '*.cpp' | xargs clang-tidy -p build -fix
