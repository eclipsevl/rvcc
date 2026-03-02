# rvcc — Minimal C Compiler for uvm32 (RV32IM)

rvcc is a self-contained C compiler targeting the [uvm32](https://github.com/ringtailsoftware/uvm32) RISC-V virtual machine. It compiles a substantial subset of C directly to RV32IM flat binaries — no external toolchain, assembler, or linker required. Designed to be embedded into host applications via a simple C API.

## Usage

```
rvcc [-O] [-I dir] [-H name] [-o output] input.c
```

| Flag | Description |
|------|-------------|
| `-O` | Enable peephole optimizer |
| `-I dir` | Add include search directory (repeatable) |
| `-H name` | Generate C header (uint8_t array) instead of binary |
| `-o file` | Output file (default: `input.bin`) |

### Examples

```bash
# Compile hello world
./rvcc -O -I . -o hello.bin hello.c

# Generate embeddable C header
./rvcc -O -I . -H hello_rom -o hello.h hello.c
```

## Embedding API

rvcc can be embedded into C/C++ applications without stdio dependency:

```c
#include "rvcc.h"

rvcc_result_t result;
bool ok = rvcc_compile(source, "main.c", my_file_reader, NULL,
                       RVCC_OPT, &result);
if (ok) {
    // result.binary contains the flat binary
    // result.binary_len is its length
}
rvcc_result_free(&result);
```

File I/O is handled through a callback: `char *reader(const char *path, void *userdata)`.

## Supported C Features

### Types

| Type | Size | Notes |
|------|------|-------|
| `void` | 0 | |
| `char` / `unsigned char` | 1 | |
| `short` / `unsigned short` | 2 | |
| `int` / `unsigned int` | 4 | |
| `long` | 4 | Same as int (RV32 ILP32) |
| Pointers | 4 | Single and multi-level (`int *`, `char **`) |
| Arrays | N * elem | Fixed-size, decay to pointers |
| Structs | aligned | Members with `.` and `->` access |

Type qualifiers `const`, `static`, `signed`, `unsigned` are recognized. `typedef` is supported.

### Operators

- **Arithmetic:** `+` `-` `*` `/` `%` (signed and unsigned)
- **Bitwise:** `&` `|` `^` `~` `<<` `>>`
- **Logical:** `&&` `||` `!` (with short-circuit evaluation)
- **Comparison:** `<` `>` `<=` `>=` `==` `!=`
- **Assignment:** `=` `+=` `-=` `*=` `/=` `%=` `&=` `|=` `^=` `<<=` `>>=`
- **Unary:** `++` `--` `&` (address-of) `*` (dereference) `-` (negate) `~` `!`
- **Other:** `sizeof`, type casts `(type)expr`, array indexing `[]`, struct access `.` `->`

### Control Flow

- `if` / `else`
- `while`, `for` (with init declarations)
- `switch` / `case` / `default` (with fall-through and `break`)
- `break`, `continue`, `return`
- Block scoping `{ }`

### Declarations

- Local variables with initializers: `int x = 5;`
- Array initializers: `int arr[] = {1, 2, 3};` and `char buf[] = "str";`
- Multiple declarations: `int a, b, c;`
- Global variables (constant initializers)
- Function definitions with up to 8 parameters
- Forward function references (auto-resolved)

### Preprocessor

- `#define` / `#undef` — object-like and function-like macros (up to 16 params)
- `#include "file"` — file inclusion (up to 8 levels deep)
- `#ifdef` / `#ifndef` / `#if` / `#elif` / `#else` / `#endif`
- `defined()` operator in `#if` expressions
- `//` and `/* */` comments

### Literals

- Decimal: `123`, hex: `0xFF`, octal: `0777`
- Character: `'a'`, `'\n'`, `'\x41'`
- String: `"hello"` with escape sequences, adjacent literal concatenation
- Type suffixes (`U`, `L`) recognized and ignored

### Built-in: `__syscall`

```c
int result = __syscall(id, p1, p2);
```

Direct syscall to the uvm32 host. Maps to `a7=id, a0=p1, a1=p2, ecall, result=a2`. Wrapped by macros in `rvcc_target.h`:

```c
println("Hello");       // UVM32_SYSCALL_PRINTLN
print("no newline");    // UVM32_SYSCALL_PRINT
printdec(42);           // UVM32_SYSCALL_PRINTDEC
printhex(0xDE);         // UVM32_SYSCALL_PRINTHEX
putc('A');              // UVM32_SYSCALL_PUTC
int ch = getc();        // UVM32_SYSCALL_GETC
yield();                // UVM32_SYSCALL_YIELD
i2c_write(addr, buf, len);
i2c_read(addr, buf, wlen, rlen);
```

## Not Supported

- **Types:** `float`, `double`, `enum`, `union`, multi-dimensional arrays, VLAs
- **Expressions:** ternary `?:`, comma operator, function pointers
- **Statements:** `do-while`, `goto`, labels
- **Functions:** variadic args (`...` parsed but not usable), function pointers
- **Storage:** `extern`, `volatile`, `register`
- **Preprocessor:** `#pragma`, `#error`, stringification `#`, token pasting `##`, `#include <...>`
- **Other:** inline assembly, attributes, complex global initializers

## Optimizer (`-O`)

The peephole optimizer runs multiple passes over the generated code:

| Pattern | Description |
|---------|-------------|
| Push-pop elimination | Replaces stack traffic with register moves |
| NOP removal | Removes patched-out prologue slots |
| Dead move elimination | Removes `mv rX, rY` when rX is immediately overwritten |
| Jump-to-next removal | Removes `j +4` (jump over nothing) |
| Compare-branch fusion | Merges `sub+sltiu+beq` into single branch |
| Relational fusion | Merges `slt+beq` into `bge`/`blt` |
| Leaf function detection | Skips ra save/restore for functions that don't call others |
| Dead function elimination | Removes unreferenced functions (e.g. unused HAL from headers) |

### Size comparison (hello world)

| | Code size |
|---|---|
| rvcc (no opt) | 384 bytes |
| rvcc -O | 52 bytes |
| GCC -Os | 60 bytes |

## Binary Output Format

```
Offset  Content
0x00    sw   ra, 12(sp)       ─┐
0x04    jal  ra, main          │ 16-byte startup
0x08    lui  a7, 0x1000        │
0x0C    ecall (halt)          ─┘
0x10    ... function code ...
        ... data section (strings, globals) ...
```

RAM base: `0x80000000`. Code and data are contiguous.

## Limits

| Resource | Maximum |
|----------|---------|
| Local variables | 256 |
| Global variables | 128 |
| Functions | 128 |
| Function parameters | 8 |
| String literals | 256 |
| Structs | 64 |
| Struct members | 32 |
| Macros | 256 |
| Macro parameters | 16 |
| Include nesting | 8 |
| Identifier length | 64 chars |

## Architecture

| File | Purpose |
|------|---------|
| `rvcc_lex.c` | Lexer, preprocessor, token generation |
| `rvcc_type.c` | Type system, symbol tables |
| `rvcc_expr.c` | Expression parsing and codegen |
| `rvcc_stmt.c` | Statements, control flow, function definitions |
| `rvcc_emit.c` | RV32IM instruction encoding |
| `rvcc_opt.c` | Peephole optimizer |
| `rvcc_api.c` | Public API, binary finalization |
| `rvcc_main.c` | CLI driver |

## Tests

```bash
make test    # builds compiler + disassembler + test runner, runs all tests
```

Tests compile each `.c` file twice (normal and `-O`), run in a standalone RV32IM emulator, and compare output. Size comparison is printed at the end.
