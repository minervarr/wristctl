CC ?= cc
CXX ?= c++
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O2
CPPFLAGS := -Iinclude
BUILD := build

SRCS := src/generated.c src/models.c
OBJS := $(SRCS:src/%.c=$(BUILD)/%.o)

.PHONY: all test clean

all: $(BUILD)/test_vectors $(BUILD)/test_models $(BUILD)/test_cpp

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/test_vectors: tests/test_vectors.c tests/generated_vectors.h $(OBJS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_vectors.c $(OBJS) -o $@

$(BUILD)/test_models: tests/test_models.c $(OBJS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_models.c $(OBJS) -o $@

$(BUILD)/test_cpp: tests/test_cpp.cc $(OBJS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_cpp.cc $(OBJS) -o $@

test: all
	./$(BUILD)/test_vectors
	./$(BUILD)/test_models
	./$(BUILD)/test_cpp

clean:
	rm -rf $(BUILD)
