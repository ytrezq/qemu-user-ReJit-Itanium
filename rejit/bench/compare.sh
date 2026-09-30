#!/bin/bash
# Compare deux qemu-user sur les charges de work.sh, dans le rootfs de la
# cible, via binfmt_misc et un overlay jetable (le rootfs n'est pas modifié).
# Exécutions alternées A, B, A, B... (3 chacune), temps minimum de chaque ;
# vérifie que les sorties sont identiques.
#
#   sudo ./compare.sh <ppc64le|aarch64|armhf|riscv64> <qemu-A> <qemu-B> [charges...]
#
# Charges : python pyfp gzip xz sort sha awk grep rpm dnf (défaut : toutes).
# Sur armhf et riscv64 (Ubuntu), rpm et dnf sont remplacés par dpkg et
# apt-cache.
# Rootfs : variable ROOTFS, sinon ../rootfs-<cible> (voir chroot-demo.sh).  Sorties des
# charges : .bench-<cible>/out.
set -e
HERE=$(dirname "$(readlink -f "$0")")
case "$1" in
ppc64le) qt=ppc64le ;;
aarch64) qt=aarch64 ;;
armhf|arm) qt=arm; set -- armhf "${@:2}" ;;
riscv64) qt=riscv64 ;;
*) echo "usage : $0 <ppc64le|aarch64|armhf|riscv64> <qemu-A> <qemu-B> [charges]" >&2; exit 1 ;;
esac
arch=$1
A=$(readlink -f "$2")
B=$(readlink -f "$3")
shift 3
ws=${*:-python pyfp gzip xz sort sha awk grep rpm dnf}
ROOTFS=$(readlink -f "${ROOTFS:-$HERE/../rootfs-$arch}")
[ -d "$ROOTFS/usr" ] || {
    echo "rootfs $arch introuvable : $ROOTFS (variable ROOTFS)" >&2
    exit 1
}
W=${WORK:-$HERE/.bench-$arch}
M=$W/merged
OUT=$W/out

mkdir -p $W/upper $W/work $M $OUT
mountpoint -q $M || mount -t overlay overlay \
    -o lowerdir=$ROOTFS,upperdir=$W/upper,workdir=$W/work $M
mountpoint -q $M/proc || mount -t proc proc $M/proc
mountpoint -q $M/dev || mount --bind /dev $M/dev
cleanup() {
    for m in $M/dev $M/proc $M; do
        if mountpoint -q $m; then umount $m; fi
    done
    # binfmt : revenir au JIT de bin/
    "$HERE/../install-binfmt.sh" $qt > /dev/null
}
trap cleanup EXIT
cp -f $HERE/work.sh $M/tmp/work.sh
chmod +x $M/tmp/work.sh

use() { QEMU=$1 "$HERE/../install-binfmt.sh" $qt > /dev/null; }

use "$A"
chroot $M /tmp/work.sh none > /dev/null 2>&1 || true    # fichiers d'entrée
printf '%-7s %-7s %9s %9s %7s\n' "" charge A B "A/B"
for w in $ws; do
    ta=(); tb=()
    for i in 1 2 3; do
        for q in A B; do
            if [ $q = A ]; then use "$A"; else use "$B"; fi
            s=$(date +%s.%N)
            chroot $M /tmp/work.sh $w > $OUT/$q-$w.txt 2>&1 || true
            e=$(date +%s.%N)
            t=$(echo "$e - $s" | bc)
            if [ $q = A ]; then ta+=($t); else tb+=($t); fi
        done
    done
    ma=$(printf '%s\n' "${ta[@]}" | sort -g | head -1)
    mb=$(printf '%s\n' "${tb[@]}" | sort -g | head -1)
    same=$(cmp -s $OUT/A-$w.txt $OUT/B-$w.txt && echo "" || echo " (sorties différentes)")
    printf '%-7s %-7s %8.2fs %8.2fs %6.2fx%s\n' $arch $w $ma $mb \
        $(echo "$ma / $mb" | bc -l) "$same"
done
