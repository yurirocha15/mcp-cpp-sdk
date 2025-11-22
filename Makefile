.PHONY: all init init-dev build test clean format lint

SUDO := $(shell [ $$(id -u) -eq 0 ] || echo "sudo")
# use half of the number of cores
NUM_CPU_2 := $(shell nproc | awk '{print $$1/2}')

all: build

init:
	$(SUDO) apt-get update && $(SUDO) apt-get install -y python3-pip pipx
	pipx ensurepath
	pipx install conan
	conan profile detect --force

init-dev: init
	$(SUDO) apt-get update && $(SUDO) apt-get install -y clang-format clang-tidy
	pipx install pre-commit
	pre-commit install

build:
	conan install . --output-folder=build --build=missing -s compiler.cppstd=20
	cmake --preset conan-release
	cmake --build --preset conan-release -j$(NUM_CPU_2)

test:
	ctest --preset conan-release -j$(NUM_CPU_2)

clean:
	rm -rf build

format:
	find include src test examples -name '*.hpp' -o -name '*.cpp' | xargs clang-format -i

lint:
	find include src test examples -name '*.hpp' -o -name '*.cpp' | xargs clang-tidy -p build -fix
