#!/bin/bash
# Tests différentiels RISC-V (riscv64) : compare le qemu-user JIT à un QEMU
# de référence (non modifié), sorties comparées octet par octet ; les tests
# vectoriels à VLEN 128, 256 et 512 (-cpu max,vlen=N).
#
#   ./run-tests.sh /chemin/ref/qemu-riscv64 [/chemin/jit/qemu-riscv64]
#
# Compilateur croisé : celui du paquet Debian/Ubuntu gcc-riscv64-linux-gnu
# (variable CC pour en changer ; GCC 13 ou plus récent, pour RVV 1.0).
# Les tests générés (gen_*.py) couvrent chaque EEW/SEW/LMUL, vl = VLMAX,
# en dessous, connu (vsetivli) ou non, masqué ou non.
set -e
HERE=$(dirname "$(readlink -f "$0")")
REF=$(readlink -f "$1")
JIT=$(readlink -f "${2:-$HERE/../../bin/qemu-riscv64}")
CC=${CC:-riscv64-linux-gnu-gcc}
OUT=${OUT:-$HERE/out}
mkdir -p "$OUT"
cd "$OUT"

for g in ldst ldff fp sat mv mask vfp; do
    python3 $HERE/gen_$g.py
done
$CC -O2 -march=rv64gcv -static -o rvv_ldst rvv_ldst.c
$CC -O2 -march=rv64gcv -static -o rvv_ldff rvv_ldff.c
$CC -O1 -march=rv64gc -static -o rvfp rvfp.c
$CC -O2 -march=rv64gc -static -o rvfp2 $HERE/rvfp2.c -lm -lpthread
$CC -O1 -march=rv64gcv -static -o rvsat rvsat.c
$CC -O1 -march=rv64gcv -static -o rvmv rvmv.c
$CC -O1 -march=rv64gcv -static -o rvmask rvmask.c
$CC -O1 -march=rv64gcv -static -o rvvfp rvvfp.c
$CC -O2 -march=rv64gcv -static -o smc $HERE/smc.c
$CC -O2 -march=rv64gcv -static -o vtbtest $HERE/vtbtest.c
$CC -O2 -march=rv64gcv -static -o csrtest $HERE/csrtest.c -lm
$CC -O2 -march=rv64gcv -static -o vsettest $HERE/vsettest.c

res=0
check() {       # check <nom> <test> [vlen]: exécute avec les deux QEMU, compare
    local name=$1 t=$2 cpu=()
    if [ -n "$3" ]; then
        cpu=(-cpu max,vlen=$3)
    fi
    $REF "${cpu[@]}" ./$t > $t$3.ref 2>&1 || true
    $JIT "${cpu[@]}" ./$t > $t$3.jit 2>&1 || true
    if cmp -s $t$3.ref $t$3.jit; then
        printf '%-58s identique\n' "$name"
    else
        printf '%-58s DIFFÉRENT\n' "$name"
        res=1
    fi
}
for v in 128 256 512; do
    check "rvv_ldst VLEN $v (vle/vse, fautes, vsetivli)" rvv_ldst $v
    check "rvv_ldff VLEN $v (vle*ff.v, fin de page, strlen)" rvv_ldff $v
    check "rvsat VLEN $v (vsaddu/vsadd/vssubu/vssub, vxsat)" rvsat $v
    check "rvmv VLEN $v (vmv.v.x, vmv.v.i, vid.v)" rvmv $v
    check "rvmask VLEN $v (vms*, vm*.mm, vcpop, vfirst)" rvmask $v
    check "rvvfp VLEN $v (vfadd... vfnmsub, vfmin/max, vfsqrt, fflags)" rvvfp $v
    check "vsettest VLEN $v (vsetvl*, boucles)" vsettest $v
done
check "rvfp (F et D : 226 instructions x modes d'arrondi, fflags)" rvfp
check "rvfp2 (fflags : syscalls, signaux, threads, frm)" rvfp2
check "smc (code modifié par vse8/vse32)" smc
check "vtbtest (jalr après un vsetivli dans le TB)" vtbtest
check "csrtest (lectures de frm, fflags, fcsr, vl, vtype, vlenb)" csrtest
exit $res
