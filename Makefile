NAME := sop-backup

CC ?= cc

CFLAGS := -std=c17 -Wall -Wextra -Wshadow -Wno-unused-parameter -Wno-unused-const-variable -g -O0
LDFLAGS :=
LDLIBS :=

CFLAGS  += -fsanitize=address,undefined
LDFLAGS += -fsanitize=address,undefined

ifdef CI
CFLAGS := -std=c17 -Wall -Wextra -Wshadow -Werror -Wno-unused-parameter -Wno-unused-const-variable -O2
LDFLAGS :=
endif

SOURCES := $(shell find src -type f -name '*.c')
OBJECTS := $(SOURCES:.c=.o)

.PHONY: all clean

all: $(NAME)

$(NAME): $(OBJECTS)
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)


%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(NAME) $(OBJECTS)
