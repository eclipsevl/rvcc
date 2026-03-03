#!/bin/bash
# Run rvcc test suite — both unoptimized and optimized
set -e
cd "$(dirname "$0")/.."

PASS=0
FAIL=0

run_test() {
    local src="$1"
    local expected="$2"
    local opt_flag="$3"    # "" or "-O"
    local suffix="$4"      # "" or "_opt"
    local name=$(basename "$src" .c)
    local bin="test/${name}${suffix}.bin"
    local label="${name}${suffix}"

    printf "  %-25s " "$label"

    # Compile with rvcc
    if ! ./rvcc $opt_flag -I . -I common -o "$bin" "$src" 2>/tmp/rvcc_err; then
        echo "FAIL (compile error)"
        cat /tmp/rvcc_err
        FAIL=$((FAIL + 1))
        return
    fi

    # Disassemble (non-fatal)
    local asm="test/${name}${suffix}.asm"
    if ! ./rvcc_disasm "$bin" > "$asm" 2>/dev/null; then
        echo "(disasm warning)" >&2
    fi

    # Run in test_runner
    actual=$(./test/test_runner "$bin" 2>/tmp/rvcc_err) && status=0 || status=$?

    if [ $status -ne 0 ]; then
        echo "FAIL (runtime error, exit=$status)"
        cat /tmp/rvcc_err
        FAIL=$((FAIL + 1))
        return
    fi

    if [ "$actual" = "$expected" ]; then
        echo "OK"
        PASS=$((PASS + 1))
    else
        echo "FAIL"
        echo "    expected: $(echo "$expected" | head -1)"
        echo "    got:      $(echo "$actual" | head -1)"
        FAIL=$((FAIL + 1))
    fi
}

run_pair() {
    local src="$1"
    local expected="$2"
    run_test "$src" "$expected" "" ""
    run_test "$src" "$expected" "-O" "_opt"
}

echo "rvcc test suite"
echo "==============="

run_pair test/test_hello.c "Hello world"
run_pair test/test_array.c "$(printf 'DE AD')"
run_pair test/test_func.c "7"
run_pair test/test_ret.c "5"
run_pair test/test_ifelse.c "$(printf 'x is 10\nx <= 20\nx is 10 again\n0 < x < 100\n5 < x < 15\nx is 1 or 10')"
run_pair test/test_loops.c "$(printf '10 10 5 5')"
run_pair test/test_switch.c "$(printf 'x is 2\ny is default\nz is 1 or 2\nw+5 is 15')"
run_pair test/test_eeprom.c "$(printf 'EEPROM write 4 bytes at 0x00\nWrite OK\nEEPROM read 4 bytes from 0x00\nRead: DE AD BE EF')"
run_pair test/test_pointer.c "$(printf '42 99 10 30 65 66')"
run_pair test/test_recursion.c "$(printf '120 13 55')"
run_pair test/test_struct.c "$(printf '3 4 7 30')"
run_pair test/test_global.c "$(printf '0 3 10 600')"
run_pair test/test_arith.c "$(printf '56 14 2 15 165 240 16 32 255 -42')"
run_pair test/test_cast.c "$(printf '65 -5 200 66 77')"
run_pair test/test_compound.c "$(printf '15 12 24 4 1 15 63 48 32 8 11 10')"
run_pair test/test_string.c "$(printf 'test\n65 68 5 9 10 65')"
run_pair test/test_nested.c "$(printf '9 5 64 3 1111')"
run_pair test/test_multi_func.c "$(printf '49 15 9 3 10 0 15')"

echo ""
echo "Results: $PASS passed, $FAIL failed"

# Show size comparison
echo ""
echo "Size comparison (bytes):"
printf "  %-20s %8s %8s %6s\n" "Test" "Normal" "Opt" "Saved"
for src in test/test_*.c; do
    name=$(basename "$src" .c)
    norm="test/${name}.bin"
    opt="test/${name}_opt.bin"
    if [ -f "$norm" ] && [ -f "$opt" ]; then
        sz_norm=$(wc -c < "$norm" | tr -d ' ')
        sz_opt=$(wc -c < "$opt" | tr -d ' ')
        saved=$((sz_norm - sz_opt))
        printf "  %-20s %8d %8d %6d\n" "$name" "$sz_norm" "$sz_opt" "$saved"
    fi
done

[ $FAIL -eq 0 ] || exit 1
