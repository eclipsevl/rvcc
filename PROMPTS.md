# rvcc Development Prompts

Step-by-step prompts used to build rvcc with Claude Code, from initial VM integration to final optimizer tuning.

---

## Session 1 — VM Integration

**1.** I want to add a virtual machine to the project, I want to use https://github.com/ringtailsoftware/uvm32/tree/main. Integrate it into my project.

**2.** In /tools create a folder with makefile to build the binary for virtual machine. Make hello world example.

**3.** Use the makefile example from repository https://github.com/ringtailsoftware/uvm32/tree/main

**4.** Convert the binary into c header array uint8_t.

**5.** Add second configuration to the mplab project: debug. In it, disable the linker file, remove loadable.

## Session 2 — I2C Syscalls and EEPROM Example

**6.** I need to add more syscalls. First one would be `bool i2c_write(uint8_t dev_addr, uint8_t* buf, uint8_t len)`.

**7.** Make an example for the VM that writes to eeprom at addr 0x50 and then reads back. Compile, make .h.

**8.** Use I2C_WriteRead for i2c_read because we need to write the addr where we are reading from.

**9.** Commit and push.

**10.** Continue, also implement all the declared syscalls.

## Session 3 — Embedded C Compiler (first attempts)

**11.** I need to embed C compiler into my C/C++ config tool. Please create in /tools a minimal compiler and linker for my vm code.

> Claude proposed a full plan. User rejected it as too complex.

**12.** Make a minimalist compiler, only compile simple code like hello world and eeprom example.

> Another attempt, still too complex.

**13.** In rvcc folder make a minimal c-like compiler for the risc-v vm. I only need very minimal compiler.

**14.** Use tinycc to make a minimal c-compiler for my risc-v vm.

> This approach was also abandoned.

## Session 4 — rvcc Implementation (success)

**15.** *(Detailed plan provided by user)*

Implement the following plan:

### rvcc -- Minimal C Compiler for uvm32 (RV32IM)

Multi-file architecture: rvcc_emit.c, rvcc_lex.c, rvcc_type.c, rvcc_expr.c, rvcc_stmt.c, rvcc_api.c, rvcc_main.c. Types: void, char, short, int, unsigned variants, pointers, arrays, structs. Statements: if/else, while, for, switch/case, return, break, continue. Preprocessor: #define, #include, #ifdef/#ifndef/#if/#else/#endif. Built-in __syscall(id, p1, p2). Stack-based codegen model. Embeddable API with file reader callback. Test infrastructure with standalone VM runner.

> This was the plan that succeeded. rvcc was built in one session.

## Session 5 — Tests and Disassembler

**16.** Please add more tests for basic C support: if-else, switch case.

**17.** Good. Now please add disassemble in the tests, save as .asm.

> A standalone RV32IM disassembler (`rvcc_disasm.c`) was created and integrated into the test pipeline.

## Session 6 — Peephole Optimizer

**18.** Good. Now, let's add optimization option to the compiler. The output binary is very long.

> User provided a detailed plan for a peephole optimizer with `-O` flag. Patterns: push-pop elimination, jump-to-next removal, compare-branch fusion. Algorithm: decode → mark → offset map → fix branches → compact → remap patches.

**19.** Good but not enough. Hello world example uses so many load/save instructions. Why?

**20.** Try compilation for the vm with gcc, compare result with rvcc. What can be improved?

> Claude compiled hello.c with both GCC -Os and rvcc -O, compared disassembly side-by-side, and identified the gap: GCC 60 bytes vs rvcc 360 bytes.

**21.** Do the improvements, show result comparison.

> Generalized push-pop patterns and additional compare-branch fusion patterns were added. Hello went from 360 to 264 bytes.

## Session 7 — Codegen Improvements (closing the gap with GCC)

**22.** *(Detailed plan provided by user)*

Implement the following plan:

### rvcc Codegen Improvements (close gap with GCC -Os)

Four improvements:
1. Direct-register `__syscall` codegen — evaluate args into a7/a0/a1 directly, zero stack traffic.
2. Leaf function detection — skip ra save/restore when function makes no calls.
3. Dead function elimination — remove unreferenced functions from binary.
4. NOP removal and dead move elimination — new optimizer patterns.

> Result: hello_opt went from 360 to 72 bytes (GCC -Os is 60 bytes).

## Optimization Progress

### Step 1 — Peephole optimizer (prompt 18)

Push-pop elimination, j+4 removal, compare-branch fusion.

| Test | No-opt | `-O` | Saved |
|------|--------|------|-------|
| hello | 512 | 408 | 20% |
| ret | 617 | 497 | 19% |
| func | 689 | 545 | 21% |
| array | 865 | 653 | 25% |
| ifelse | 1657 | 1257 | 24% |
| loops | 1549 | 1157 | 25% |
| switch | 1553 | 1305 | 16% |
| eeprom | 2257 | 1757 | 22% |

### Step 2 — Generalized push-pop (prompt 21)

More compare-branch patterns, generalized push-pop with middle instructions.

| Test | No-opt | `-O` | Saved |
|------|--------|------|-------|
| hello | 512 | 360 | 30% |
| ret | 617 | 429 | 30% |
| func | 689 | 469 | 32% |
| array | 865 | 545 | 37% |
| ifelse | 1657 | 1029 | 38% |
| loops | 1549 | 969 | 37% |
| switch | 1553 | 1077 | 31% |
| eeprom | 2257 | 1357 | 40% |

### Step 3 — Direct syscall, leaf functions, dead fn elimination (prompt 22)

Direct-register `__syscall` codegen, leaf function detection (skip ra save/restore), dead function elimination, NOP/dead-move removal.

| Test | No-opt | `-O` | Saved |
|------|--------|------|-------|
| hello | 396 | **72** | 82% |
| ret | 469 | 137 | 71% |
| func | 549 | 197 | 64% |
| array | 661 | 265 | 60% |
| ifelse | 1261 | 741 | 41% |
| loops | 1217 | 673 | 45% |
| switch | 1157 | 793 | 31% |
| eeprom | 1705 | 1277 | 25% |

### Step 4 — Store-load elimination, switch in t-register (prompt 26)

Store-load elimination, switch value in t-register instead of stack, cast-constant recognition in `__syscall`.

| Test | No-opt | `-O` | Saved |
|------|--------|------|-------|
| hello | 384 | **60** | 84% |
| ret | 445 | 113 | 75% |
| func | 525 | 173 | 67% |
| array | 613 | 217 | 65% |
| ifelse | 1141 | 617 | 46% |
| loops | 1121 | 565 | 50% |
| switch | 953 | 577 | 39% |
| eeprom | 1525 | 1097 | 28% |

### Step 5 — Full t-register usage, li/lui fusion, array fast path (prompt 27)

Push-pop renaming through t0-t6, li-mv/lui-mv fusion, constant-index array element fast path.

| Test | No-opt | `-O` | Saved |
|------|--------|------|-------|
| hello | 384 | **52** | 86% |
| ret | 445 | 101 | 77% |
| func | 525 | 153 | 71% |
| array | 469 | 125 | 73% |
| ifelse | 1141 | 537 | 53% |
| loops | 1121 | 529 | 53% |
| switch | 953 | 497 | 48% |
| eeprom | 1149 | 793 | 31% |

### Summary — optimized size across steps (bytes)

| Test | Step 1 | Step 2 | Step 3 | Step 4 | Step 5 | Total saved |
|------|--------|--------|--------|--------|--------|-------------|
| hello | 408 | 360 | 72 | 60 | **52** | 87% |
| ret | 497 | 429 | 137 | 113 | **101** | 80% |
| func | 545 | 469 | 197 | 173 | **153** | 72% |
| array | 653 | 545 | 265 | 217 | **125** | 81% |
| ifelse | 1257 | 1029 | 741 | 617 | **537** | 57% |
| loops | 1157 | 969 | 673 | 565 | **529** | 54% |
| switch | 1305 | 1077 | 793 | 577 | **497** | 62% |
| eeprom | 1757 | 1357 | 1277 | 1097 | **793** | 55% |

GCC -Os hello world: 60 bytes. rvcc -O: **52 bytes**.

## Session 8 — Final touches

**23.** Add the eeprom test to list of tests, update gitignore to not track /test/*.bin, *.asm.

**24.** Write readme.md for this compiler. Describe what is supported and what is not, mention this is compiler for uvm32 vm. In a separate file write all the prompts I made to you, step-by-step.

## Session 9 — Register-based micro-optimizations

**25.** I see the asm code, not many t registers are used, mostly a0/a1 and stack. Do you think using t register can help reduce the machine code length?

> Claude analyzed the switch disassembly and identified three quick wins: store-load elimination, switch value in t-register, cast-constant recognition.

**26.** Do first 3 for now, let's see the difference before/after. Update the prompts.md.

**27.** There are t0-t7 registers, not only t0 and t1 used. Use all of them.

> Expanded t-register usage across the optimizer: push-pop renaming now tries all t0-t6 instead of just t0. Added li-mv fusion (`li a0, X; mv rY, a0` → `li rY, X`) and lui-mv fusion patterns. Added constant-index array element fast path in codegen (`buf[2]` computes offset at compile time). Hello world: **52 bytes** — now smaller than GCC -Os (60 bytes).
