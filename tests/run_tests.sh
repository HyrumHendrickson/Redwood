#!/bin/sh
# Regression tests for redwood-cli. Usage: tests/run_tests.sh [path-to-redwood-cli]
set -u

CLI=${1:-./redwood-cli}
fail=0

pass() { printf 'ok   - %s\n' "$1"; }
bad()  { printf 'FAIL - %s\n' "$1"; fail=1; }

# Every coin must conserve supply and report no mismatch.
for coin in 0 1 2; do
    out=$("$CLI" "$coin" 3000 1) || { bad "coin $coin exits cleanly"; continue; }
    case $out in
        *MISMATCH*) bad "coin $coin supply invariant" ;;
        *"OK"*)     pass "coin $coin supply invariant" ;;
        *)          bad "coin $coin produced no supply check" ;;
    esac
done

# Runs with the same seed and settings must be byte-identical.
a=$("$CLI" 0 3000 7 0:2:1500) && b=$("$CLI" 0 3000 7 0:2:1500)
[ "$a" = "$b" ] && pass "deterministic for equal seeds" || bad "deterministic for equal seeds"

# Different seeds should diverge.
c=$("$CLI" 0 3000 8 0:2:1500)
[ "$a" != "$c" ] && pass "different seeds diverge" || bad "different seeds diverge"

# Every cheat bit must keep the supply invariant intact.
for mask in 1 2 4 8 16 32 64 127; do
    out=$("$CLI" 0 3000 3 "0:$mask:600") || { bad "cheat mask $mask exits cleanly"; continue; }
    case $out in
        *MISMATCH*) bad "cheat mask $mask supply invariant" ;;
        *)          pass "cheat mask $mask supply invariant" ;;
    esac
done

# Invalid input must be rejected with exit status 2, never crash.
expect_usage_error() {
    "$CLI" "$@" >/dev/null 2>&1
    rc=$?
    [ "$rc" -eq 2 ] && pass "rejects: $*" || bad "rejects: $* (exit $rc)"
}
expect_usage_error -1 10
expect_usage_error 3 10
expect_usage_error 0 abc
expect_usage_error 0 -5
expect_usage_error 0 10 notaseed
expect_usage_error 0 10 1 99:1
expect_usage_error 0 10 1 0:999
expect_usage_error 0 10 1 garbage

[ "$fail" -eq 0 ] && echo "all tests passed" || echo "tests failed"
exit "$fail"
