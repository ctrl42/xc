BUILD=bin

# C compiler
CC=clang
CFLAGS=-std=c2x -Wall -Wextra -Werror -Wshadow

# General
C_SOURCES=xc.c

.PHONY: xc
xc: ${C_SOURCES}
	@mkdir -p $(BUILD)
	@$(CC) $(CFLAGS) $< -o $(BUILD)/$@

clean:
	@rm -rf $(BUILD)
