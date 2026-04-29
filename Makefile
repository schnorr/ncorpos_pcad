# NCorpos @ PCAD – Makefile
# Inspired by Fractal @ PCAD Makefile

# Raylib installation prefix (override on command line: make RAYLIB=/path/to/raylib)
RAYLIB ?= /usr

ifndef LOG_LEVEL
LOG_LEVEL = LOG_FULL
endif

CC    = gcc
MPICC = mpicc

CFLAGS  = -Wall -Wextra -g \
          -Iinclude -Isrc \
          -I$(RAYLIB)/include \
          -pthread \
          -DLOG_LEVEL=$(LOG_LEVEL)

SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin

# All source files except the three binaries that each provide main()
SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

# Objects that belong exclusively to one binary
GRAFICA_OBJ     := $(OBJ_DIR)/grafica.o
TEXTUAL_OBJ     := $(OBJ_DIR)/textual.o
COORDINATOR_OBJ := $(OBJ_DIR)/coordinator.o

# Shared objects (everything except the three main-owning objects)
SHARED_OBJS := $(filter-out $(GRAFICA_OBJ) $(TEXTUAL_OBJ) $(COORDINATOR_OBJ), $(OBJS))

.PHONY: all grafica textual coordinator clean

all: grafica textual coordinator

# --- Compilation rule (use mpicc for everything so MPI headers resolve) ---
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(OBJ_DIR)
	$(MPICC) $(CFLAGS) -c $< -o $@

# --- textual client (no raylib, no MPI runtime – just MPI headers) ---
textual: $(SHARED_OBJS) $(TEXTUAL_OBJ)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/textual $^ -lm -lmpi

# --- graphical client (raylib + MPI headers) ---
grafica: $(SHARED_OBJS) $(GRAFICA_OBJ)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/grafica $^ \
	    -L$(RAYLIB)/lib -Wl,-rpath,$(RAYLIB)/lib -lraylib -lm -lmpi

# --- coordinator (MPI program) ---
coordinator: $(SHARED_OBJS) $(COORDINATOR_OBJ)
	@mkdir -p $(BIN_DIR)
	$(MPICC) $(CFLAGS) -o $(BIN_DIR)/coordinator $^ -lm

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)
