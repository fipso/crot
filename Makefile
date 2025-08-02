# Makefile for TikTok Reel Animation Framework
# Using Raylib for rendering and FFmpeg libs for video encoding

# Compiler and base flags
CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2 $(shell pkg-config --cflags raylib libavcodec libavformat libavutil libswscale)
LDFLAGS = $(shell pkg-config --libs raylib libavcodec libavformat libavutil libswscale) -lm -lpthread -ldl

# Source files (expand as you add more)
SRCS = main.c
OBJS = $(SRCS:.c=.o)

# Output executable
TARGET = reel_framework

# Default target
all: $(TARGET)

# Link objects to executable
$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)

# Compile source to objects
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up
clean:
	rm -f $(OBJS) $(TARGET)

# Phony targets
.PHONY: all clean
