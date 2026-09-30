#!/bin/bash
# Tests différentiels : compare le qemu-user JIT à un QEMU de référence
# (non modifié) sur des millions de vecteurs de test FP/VSX/MMA/Altivec.
#
#   ./run-tests.sh /chemin/qemu-reference [/chemin/qemu-jit]
#
# Compile les tests avec une chaîne croisée native x86 -> Power (variable CC,
# défaut celle du paquet Debian/Ubuntu gcc-powerpc64le-linux-gnu ; le GCC
# compilé pour Power11 convient aussi), les exécute avec
# les deux QEMU (processeur power11) et compare avec fpcmp.py, qui accepte
# les écarts documentés dans le README.
set -e
HERE=$(dirname "$(readlink -f "$0")")
REF=$(readlink -f "$1")
JIT=$(readlink -f "${2:-$HERE/../../bin/qemu-ppc64le}")
CC=${CC:-powerpc64le-linux-gnu-gcc}
OUT=${OUT:-$HERE/out}
mkdir -p $OUT
cd $OUT
for t in fptest vsxtest vmxtest mmatest segv; do
    $CC -O2 -static -mcpu=power10 -o $t $HERE/$t.c -lm -lpthread
    QEMU_CPU=power11 $REF ./$t > $t.ref 2>&1 || true
    QEMU_CPU=power11 $JIT ./$t > $t.jit 2>&1 || true
done
echo "== fptest (FP classique, séquences, signaux, threads)"
python3 $HERE/fpcmp.py fptest.ref fptest.jit | tail -1
echo "== vsxtest (VSX scalaire et vectoriel)"
python3 $HERE/fpcmp.py vsxtest.ref vsxtest.jit | tail -1
echo "== vmxtest (vshasigma, vsldoi, vperm, xxperm, lxvl/stxvl)"
cmp -s vmxtest.ref vmxtest.jit && echo "identique" || echo "DIFFÉRENT"
echo "== mmatest (MMA xvf32ger*, xvf64ger*)"
python3 $HERE/fpcmp.py mmatest.ref mmatest.jit | tail -1
echo "== segv (fautes de lxvl/stxvl)"
cmp -s segv.ref segv.jit && echo "identique" || echo "DIFFÉRENT"
