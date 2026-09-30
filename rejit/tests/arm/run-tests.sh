#!/bin/bash
# Tests différentiels AArch64 et AArch32 : compare le qemu-user JIT à un QEMU
# de référence (non modifié), sorties comparées octet par octet.
#
#   ./run-tests.sh /chemin/ref/qemu-aarch64 /chemin/ref/qemu-arm \
#                  [/chemin/jit/qemu-aarch64 /chemin/jit/qemu-arm]
#
# Compilateurs croisés : ceux des paquets Debian/Ubuntu gcc-aarch64-linux-gnu
# et gcc-arm-linux-gnueabihf (variables CC64 et CC32 pour en changer).
# QEMU_RAND_SEED fixe les clés PAuth et AT_RANDOM, pour des sorties
# reproductibles.
set -e
HERE=$(dirname "$(readlink -f "$0")")
REF64=$(readlink -f "$1")
REF32=$(readlink -f "$2")
JIT64=$(readlink -f "${3:-$HERE/../../bin/qemu-aarch64}")
JIT32=$(readlink -f "${4:-$HERE/../../bin/qemu-arm}")
CC64=${CC64:-aarch64-linux-gnu-gcc}
CC32=${CC32:-arm-linux-gnueabihf-gcc}
OUT=${OUT:-$HERE/out}
mkdir -p "$OUT"
cd "$OUT"

S=$HERE
$CC64 -O1 -static -march=armv8.2-a -o fptest-a64 $S/fptest-a64.c -lpthread
$CC32 -O1 -static -march=armv8-a -mfpu=neon-fp-armv8 -mthumb -o fptest-a32 $S/fptest-a32.c -lpthread
$CC64 -O2 -static -march=armv8.3-a -o pactest $S/pactest.c
$CC64 -O1 -static -march=armv8.5-a -mbranch-protection=none -o pactest2 $S/pactest2.c
$CC64 -O1 -static -march=armv8.8-a+mops -o mopstest $S/mopstest.c
$CC64 -O2 -static -march=armv8.2-a+crypto -o shatest $S/shatest.c
$CC64 -O2 -static -o pairtest-a64 $S/pairtest.c
$CC32 -O2 -static -march=armv7-a -mfpu=neon -o pairtest-a32 $S/pairtest.c
$CC32 -O2 -static -march=armv7-a+fp -mthumb -o simdtest $S/simdtest.c

export QEMU_RAND_SEED=1
res() {
    if cmp -s "$1" "$2"; then echo "identique"; else echo "DIFFÉRENT"; fail=1; fi
}
fail=0

run2() {    # run2 <qemu-ref> <qemu-jit> <binaire> [args...]
    local r=$1 j=$2 b=$3; shift 3
    "$r" ./$b "$@" > $b.ref 2>&1 || true
    "$j" ./$b "$@" > $b.jit 2>&1 || true
}

echo "== fptest-a64 (FP scalaire S/D, vecteurs 4S/2D, FMA, conversions, FPSR/NZCV)"
run2 $REF64 $JIT64 fptest-a64
python3 $S/cmp.py fptest-a64.ref fptest-a64.jit | head -1
res fptest-a64.ref fptest-a64.jit
echo "== fptest-a32 (VFP simple et double, FPSCR)"
run2 $REF32 $JIT32 fptest-a32
python3 $S/cmp.py fptest-a32.ref fptest-a32.jit | head -1
res fptest-a32.ref fptest-a32.jit
echo "== pactest (PAC/AUT des 4 clés, pointeurs quelconques, FPAC, reset des clés)"
run2 $REF64 $JIT64 pactest
res pactest.ref pactest.jit
for m in 0 1 2; do
    QEMU_PAUTH_JIT=$m "$JIT64" ./pactest > pactest.jit$m 2>&1 || true
    printf '   QEMU_PAUTH_JIT=%d : ' $m
    res pactest.ref pactest.jit$m
done
echo "== pactest2 (cache de PAC : hits, codes faux, BRAA, RETAA, LDRAA)"
QEMU_RAND_SEED=7 "$REF64" ./pactest2 > pactest2.ref 2>&1 || true
QEMU_RAND_SEED=7 "$JIT64" ./pactest2 > pactest2.jit 2>&1 || true
res pactest2.ref pactest2.jit
echo "== mopstest (FEAT_MOPS : CPY*, CPYF*, SET*, recouvrements, fautes)"
run2 $REF64 $JIT64 mopstest
res mopstest.ref mopstest.jit
echo "== shatest (SHA256H, SHA256H2, SHA256SU0, SHA256SU1)"
run2 $REF64 $JIT64 shatest
res shatest.ref shatest.jit
echo "== pairtest (ADDP, [SU]MAXP, [SU]MINP ; VPADD, VPMAX, VPMIN)"
run2 $REF64 $JIT64 pairtest-a64
res pairtest-a64.ref pairtest-a64.jit
run2 $REF32 $JIT32 pairtest-a32
res pairtest-a32.ref pairtest-a32.jit
echo "== simdtest (A32 : [SU]ADD/SUB 8/16 et GE, SEL, écritures de NZCV)"
run2 $REF32 $JIT32 simdtest
res simdtest.ref simdtest.jit
exit $fail
