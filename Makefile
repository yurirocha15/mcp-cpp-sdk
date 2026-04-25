.PHONY: all init init-dev init-docs build debug test sanitize clean coverage docs format lint

PYTHON := $(shell command -v python3 2>/dev/null || command -v python 2>/dev/null)

all: build

init:
	$(PYTHON) scripts/init.py

init-dev:
	$(PYTHON) scripts/init.py --dev

init-docs:
	$(PYTHON) scripts/init.py --docs

build:
	$(PYTHON) scripts/build.py

debug:
	$(PYTHON) scripts/build.py --debug

test:
	$(PYTHON) scripts/build.py --test

sanitize:
	$(PYTHON) scripts/build.py --sanitize --test

coverage:
	$(PYTHON) scripts/build.py --coverage --test

docs:
	$(PYTHON) scripts/build.py --docs

clean:
	$(PYTHON) scripts/build.py --clean

format:
	find include src test examples -name '*.hpp' -o -name '*.cpp' | xargs clang-format -i

lint:
	@BUILD_DIR=$$(find build -name compile_commands.json -print -quit | xargs dirname 2>/dev/null || echo "build/release"); \
	CORES=$$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4); \
	JOBS=$$((CORES / 2 > 0 ? CORES / 2 : 1)); \
	echo "Running clang-tidy with $$JOBS parallel jobs..."; \
	find include src examples -name '*.hpp' -o -name '*.cpp' | xargs -P $$JOBS -n 1 clang-tidy -p $$BUILD_DIR --extra-arg=-I$(CURDIR)/include --header-filter='^$(CURDIR)/(include|src|examples)/.*'
