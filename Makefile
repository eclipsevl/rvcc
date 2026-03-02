CC ?= cc
CFLAGS := -Wall -Wextra -O2 -std=c99

RVCC_SRCS := rvcc_emit.c rvcc_lex.c rvcc_type.c rvcc_expr.c rvcc_stmt.c rvcc_opt.c rvcc_api.c rvcc_main.c
RVCC_HDRS := rvcc.h rvcc_internal.h

.PHONY: all clean test

all: rvcc rvcc_disasm test/test_runner

rvcc: $(RVCC_SRCS) $(RVCC_HDRS)
	$(CC) $(CFLAGS) -o $@ $(RVCC_SRCS)

rvcc_disasm: rvcc_disasm.c
	$(CC) $(CFLAGS) -o $@ $<

test/test_runner: test/test_runner.c test/mini-rv32ima.h
	$(CC) -Wall -O2 -std=c99 \
		-DMINIRV32_IMPLEMENTATION \
		-Itest \
		-o $@ $<

test: all
	@./test/run_tests.sh

clean:
	rm -f rvcc rvcc_disasm test/test_runner test/*.bin test/*.asm
