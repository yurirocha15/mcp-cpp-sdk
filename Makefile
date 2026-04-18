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
	$(PYTHON) scripts/build.py build

debug:
	$(PYTHON) scripts/build.py debug

test:
	$(PYTHON) scripts/build.py test

sanitize:
	$(PYTHON) scripts/build.py sanitize

coverage:
	$(PYTHON) scripts/build.py coverage

docs:
	$(PYTHON) scripts/build.py docs

clean:
	$(PYTHON) scripts/build.py clean

format:
	find include src test examples -name '*.hpp' -o -name '*.cpp' | xargs clang-format -i

lint:
	find include src test examples -name '*.hpp' -o -name '*.cpp' | xargs clang-tidy -p build -fix
