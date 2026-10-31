PY ?= python3
UI_PY ?= .venv/bin/python
GRADLE ?= ./gradlew
ANDROID_SERIAL ?= 192.168.101.6:44427
BUILD ?= build/linux_debug
CORE := libs/firstparty/wristctl_core
GEN := $(CORE)/gen

.PHONY: generate build test test-c test-py test-tool check clean ui ui-demo gui gui-live android android-lint android-install

generate:
	$(PY) $(GEN)/extract_models.py --check
	$(PY) $(GEN)/gen_py.py
	$(PY) $(GEN)/gen_c.py
	$(PY) $(GEN)/gen_models.py
	$(PY) $(GEN)/gen_vectors.py

# The build tool owns configure/build. `build/` is one root with one
# subdirectory per variant: linux_debug, linux, linux_native, linux_custom-*.
build:
	./scripts/build.sh build --debug

# Regenerate the committed codecs, then build Debug and run CTest plus the
# Python suites. `./scripts/build.sh test` is the same path without make.
test:
	./scripts/build.sh test

# CTest only; builds core/CLI first if the variant directory does not exist.
test-c:
	test -d $(BUILD) || ./scripts/build.sh build --debug --no-gui
	ctest --test-dir $(BUILD) --output-on-failure

test-py:
	PYTHONPATH=py $(PY) -m unittest discover -s py/tests

# The builder's own tests (paths, CLI dry-run); no toolchain needed.
test-tool:
	PYTHONPATH=scripts/src $(PY) -m unittest discover -s scripts/tests

check: test test-tool

ui:
	PYTHONPATH=py $(UI_PY) -m wristctl_proto.ui

ui-demo:
	PYTHONPATH=py $(UI_PY) -m wristctl_proto.ui --demo

# The app links the core target, so run `make test` first when the spec
# changed; otherwise the APK carries stale codecs.
gui:
	./scripts/build.sh build --debug --gui
	exec $(BUILD)/gui/wristctl_gui

gui-live:
	./scripts/build.sh build --debug --gui
	exec $(BUILD)/gui/wristctl_gui --connect

android:
	cd android && $(GRADLE) :app:assembleDebug

android-lint:
	cd android && $(GRADLE) :app:lintDebug

android-install: android
	adb -s $(ANDROID_SERIAL) install -r android/app/build/outputs/apk/debug/app-debug.apk

clean:
	rm -rf build $(CORE)/build
	find py -name '__pycache__' -type d -prune -exec rm -rf {} +
