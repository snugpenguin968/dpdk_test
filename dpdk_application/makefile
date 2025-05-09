# Compiler
CC = gcc

# Original CFLAGS
PROJECT_CFLAGS = -Wall -Wextra -Werror -pedantic -std=c11 -O2

# Get DPDK flags using pkg-config
DPDK_CFLAGS := $(shell pkg-config --cflags libdpdk)
DPDK_LIBS := $(shell pkg-config --libs libdpdk)

# Combine project CFLAGS with DPDK CFLAGS
CFLAGS = $(PROJECT_CFLAGS) $(DPDK_CFLAGS)

# Linker flags - primarily for DPDK libraries and any other specific linker options
LDFLAGS = $(DPDK_LIBS)

# Header dependencies
DEPS = udp_test.h

# Executable names
SERVER_TARGET = udp_server
CLIENT_TARGET = udp_client
TEST_DPDK_TARGET = test_dpdk

# Source files
SERVER_SOURCES = udp_server.c
CLIENT_SOURCES = udp_client.c
TEST_DPDK_SOURCES = test_dpdk.c 


# Default target
all: $(SERVER_TARGET) $(CLIENT_TARGET) test_dpdk

# Rule to build the installation test
test_dpdk: $(TEST_DPDK_SOURCES)
	$(CC) $(CFLAGS) -o $(TEST_DPDK_TARGET) $(TEST_DPDK_SOURCES) $(LDFLAGS)
	@echo "Built $(TEST_DPDK_TARGET) successfully."

# Rule to build the server
$(SERVER_TARGET): $(SERVER_SOURCES) $(DEPS)
	$(CC) $(CFLAGS) -o $(SERVER_TARGET) $(SERVER_SOURCES) $(LDFLAGS)
	@echo "Built $(SERVER_TARGET) successfully."

# Rule to build the client
$(CLIENT_TARGET): $(CLIENT_SOURCES) $(DEPS)
	$(CC) $(CFLAGS) -o $(CLIENT_TARGET) $(CLIENT_SOURCES) $(LDFLAGS)
	@echo "Built $(CLIENT_TARGET) successfully."


# Rule to clean build artifacts
clean:
	rm -f $(SERVER_TARGET) $(CLIENT_TARGET) $(TEST_DPDK_TARGET) *.o
	@echo "Clean complete."

# Phony targets are not files
.PHONY: all clean