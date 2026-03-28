.PHONY: all init init-dev build debug test clean format lint coverage

SUDO := $(shell [ $$(id -u) -eq 0 ] || echo "sudo")
# use half of the number of cores
NUM_CPU_2 := $(shell nproc | awk '{print $$1/2}')

all: build

init:
	$(SUDO) apt-get update && $(SUDO) apt-get install -y python3-pip pipx
	pipx ensurepath
	pipx install conan
	pipx install gcovr
	pipx install ninja
	conan profile detect --force

init-dev: init
	$(SUDO) apt-get update && $(SUDO) apt-get install -y clang-format clang-tidy
	pipx install pre-commit
	pre-commit install

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

clean:
	rm -rf build

format:
	find include src test examples -name '*.hpp' -o -name '*.cpp' | xargs clang-format -i

lint:
	find include src test examples -name '*.hpp' -o -name '*.cpp' | xargs clang-tidy -p build -fix
