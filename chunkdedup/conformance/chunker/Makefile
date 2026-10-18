CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror
all: chunkdedup-cli libchunkdedup.a test-chunker
chunkdedup-cli: chunkdedup_cli.o chunkdedup_chunker.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ chunkdedup_cli.o chunkdedup_chunker.o $(LDLIBS)
libchunkdedup.a: chunkdedup_chunker.o
	ar rcs $@ chunkdedup_chunker.o
chunkdedup_cli.o: chunkdedup_cli.c chunkdedup_chunker.h
chunkdedup_chunker.o: chunkdedup_chunker.c chunkdedup_chunker.h rabin64_table.h
test_chunker.o: test_chunker.c chunkdedup_chunker.h rabin64_table.h
test-chunker: test_chunker.o chunkdedup_chunker.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ test_chunker.o chunkdedup_chunker.o $(LDLIBS)
check: all
	./test-chunker
	python3 check_fixtures.py
check-oracle: all
	python3 test_conformance.py
clean:
	rm -f *.o chunkdedup-cli libchunkdedup.a test-chunker
.PHONY: all clean check check-oracle
