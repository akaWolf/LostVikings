# Makefile for Lost Vikings Seg003 Implementation

CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O2 -g
LDFLAGS = 

# Targets
TARGET = test_seg003
SOURCES = seg003_implementation.c test_seg003.c
OBJECTS = $(SOURCES:.c=.o)
HEADERS = seg003_implementation.h

# Default target
all: $(TARGET)

# Build executable
$(TARGET): $(OBJECTS)
	@echo "Linking $(TARGET)..."
	$(CC) $(OBJECTS) -o $(TARGET) $(LDFLAGS)
	@echo "✅ Build complete: $(TARGET)"

# Compile object files
%.o: %.c $(HEADERS)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Run tests
test: $(TARGET)
	@echo ""
	@echo "Running tests..."
	@echo ""
	./$(TARGET)

# Clean build artifacts
clean:
	@echo "Cleaning..."
	rm -f $(OBJECTS) $(TARGET)
	@echo "✅ Clean complete"

# Rebuild from scratch
rebuild: clean all

# Help
help:
	@echo "Lost Vikings Seg003 Implementation - Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all      - Build the test program (default)"
	@echo "  test     - Build and run tests"
	@echo "  clean    - Remove build artifacts"
	@echo "  rebuild  - Clean and rebuild"
	@echo "  help     - Show this help"
	@echo ""
	@echo "Usage:"
	@echo "  make          # Build"
	@echo "  make test     # Build and run tests"
	@echo "  make clean    # Clean"

.PHONY: all test clean rebuild help
