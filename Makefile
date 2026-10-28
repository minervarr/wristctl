PY ?= python3
UI_PY ?= .venv/bin/python
GRADLE ?= ./gradlew
BUILD ?= build
GUI_BUILD ?= build-gui
CORE := libs/firstparty/wristctl_core
GEN := $(CORE)/gen

.PHONY: generate configure build test check clean ui ui-demo gui gui-live android android-lint android-install

generate:
	$(PY) $(GEN)/extract_models.py --check
	$(PY) $(GEN)/gen_py.py
	$(PY) $(GEN)/gen_c.py
	$(PY) $(GEN)/gen_models.py
	$(PY) $(GEN)/gen_vectors.py

configure:
	cmake -S . -B $(BUILD) -G Ninja -DCMAKE_BUILD_TYPE=Debug

build: configure
	cmake --build $(BUILD)

# Generation is explicit: the committed codecs are what Android ships, and
# CMake never shells out to Python.
test: generate test-c test-py

test-c: configure
	cmake --build $(BUILD)
	ctest --test-dir $(BUILD) --output-on-failure

test-py:
	PYTHONPATH=py $(PY) -m unittest discover -s py/tests

check: test

ui:
	PYTHONPATH=py $(UI_PY) -m wristctl_proto.ui

ui-demo:
	PYTHONPATH=py $(UI_PY) -m wristctl_proto.ui --demo

# The app links the core target, so run `make test` first when the spec
# changed; otherwise the APK carries stale codecs.
gui:
	cmake -S . -B $(GUI_BUILD) -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWRISTCTL_BUILD_GUI=ON
	cmake --build $(GUI_BUILD)
	$(GUI_BUILD)/gui/wristctl_gui

gui-live:
	cmake -S . -B $(GUI_BUILD) -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWRISTCTL_BUILD_GUI=ON
	cmake --build $(GUI_BUILD)
	$(GUI_BUILD)/gui/wristctl_gui --connect

android:
	cd android && $(GRADLE) :app:assembleDebug

android-lint:
	cd android && $(GRADLE) :app:lintDebug

android-install: android
	adb install -r android/app/build/outputs/apk/debug/app-debug.apk

clean:
	rm -rf $(BUILD) $(GUI_BUILD) $(CORE)/build
	find py -name '__pycache__' -type d -prune -exec rm -rf {} +
