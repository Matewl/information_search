PYTHON ?= .venv/bin/python
.PHONY: all test sanitize integration
all:
	$(PYTHON) scripts/build.py
test: all
	build/release/l0_tests
sanitize:
	$(PYTHON) scripts/build.py --sanitize
	build/sanitize/l0_tests
integration: all
	$(PYTHON) tests/integration.py
