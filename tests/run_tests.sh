#!/usr/bin/env bash
# End-to-end tests for sender/receiver through tests/relay.py.
# Usage:  tests/run_tests.sh        (from the project root, after `make`)
set -u
cd "$(dirname "$0")/.."
OUT=tests/out
mkdir -p "$OUT"
FILE=$OUT/input.bin
python3 -c "import random; r=random.Random(337); open('$FILE','wb').write(bytes(r.getrandbits(8) for _ in range(262144)))"
ORIG=$(sha256sum "$FILE" | cut -d' ' -f1)

PORT=20000
pass=0; fail=0

# run NAME EXPECT(ok|corrupt) PROTO W K -- relay args...
run() {
    local name=$1 expect=$2 proto=$3 W=$4 k=$5; shift 5
    PORT=$((PORT + 2))
    local rport=$PORT eport=$((PORT + 1))
    local dir=$OUT/$name; mkdir -p "$dir"
    ./receiver -p "$proto" -w "$W" -k "$k" -l "$rport" -o "$dir/recv.bin" \
        -L "$dir/receiver.log" --linger 500 --idle 20000 > "$dir/receiver.out" 2>&1 &
    local rpid=$!
    python3 tests/relay.py --listen "$eport" --to "$rport" "$@" > "$dir/relay.out" 2>&1 &
    local epid=$!
    sleep 0.2
    timeout 120 ./sender -p "$proto" -w "$W" -k "$k" -f "$FILE" -P "$eport" \
        -L "$dir/sender.log" > "$dir/sender.out" 2> "$dir/sender.err"
    wait $rpid
    kill $epid 2>/dev/null; wait $epid 2>/dev/null
    local got; got=$(sha256sum "$dir/recv.bin" | cut -d' ' -f1)
    local res=corrupt; [ "$got" = "$ORIG" ] && res=ok
    local retx; retx=$(grep -c ',RETX,' "$dir/sender.log")
    local ct;   ct=$(grep ',DATA_DONE,' "$dir/sender.log" | cut -d, -f6)
    if [ "$res" = "$expect" ]; then
        pass=$((pass + 1)); tag=PASS
    else
        fail=$((fail + 1)); tag=FAIL
    fi
    printf "%-4s %-28s file=%-7s (expected %-7s) retx=%-4s time=%6.2fs\n" \
        "$tag" "$name" "$res" "$expect" "$retx" "$(echo "${ct:-0} / 1000000" | bc -l)"
}

echo "== unit tests"
./test_crc | tail -1

echo "== clean network"
for p in sw gbn sr; do run "clean_$p" ok $p 16 7; done
echo "== impairments (seeded)"
for p in sw gbn sr; do
    run "loss10_$p"    ok $p 16 7 --loss 0.10    --seed 1
    run "corrupt5_$p"  ok $p 16 7 --corrupt 0.05 --seed 2
    run "reorder5_$p"  ok $p 16 7 --reorder 0.05 --seed 3
    run "dup5_$p"      ok $p 16 7 --dup 0.05     --seed 4
    run "ackloss5_$p"  ok $p 16 7 --ackloss 0.05 --seed 5
    run "mixed_$p"     ok $p 16 7 --loss 0.05 --ackloss 0.05 --corrupt 0.05 \
                                  --dup 0.05 --reorder 0.05 --seed 6
done
echo "== small sequence space (k=3, many wrap-arounds)"
run "k3_gbn_W7_loss" ok gbn 7 3 --loss 0.05 --ackloss 0.05 --seed 7
run "k3_sr_W4_loss"  ok sr  4 3 --loss 0.05 --ackloss 0.05 --seed 7
echo "== sequence-number limits (experiment 5 idea): drop the first W ACKs"
# GBN, k=3: limit 2^k-1 = 7
run "gbn_k3_W7_safe"  ok      gbn 7 3 --drop-acks 0,1,2,3,4,5,6
run "gbn_k3_W8_break" corrupt gbn 8 3 --drop-acks 0,1,2,3,4,5,6,7
# SR, k=3: limit 2^(k-1) = 4
run "sr_k3_W4_safe"   ok      sr  4 3 --drop-acks 0,1,2,3
run "sr_k3_W5_break"  corrupt sr  5 3 --drop-acks 0,1,2,3,4
# SR, k=4: limit 8
run "sr_k4_W8_safe"   ok      sr  8 4 --drop-acks 0,1,2,3,4,5,6,7
run "sr_k4_W9_break"  corrupt sr  9 4 --drop-acks 0,1,2,3,4,5,6,7,8

echo
echo "passed: $pass  failed: $fail"
[ $fail -eq 0 ]
