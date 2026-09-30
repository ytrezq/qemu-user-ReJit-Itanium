#!/bin/bash
# chroot dans un rootfs ppc64le, aarch64, armhf ou riscv64 à travers un overlay
# jetable (le rootfs d'origine n'est jamais modifié).  Chaque execve() d'un binaire
# du rootfs y passe par binfmt_misc -> bin/qemu-<cible> (JIT).
#
#   sudo ./chroot-demo.sh aarch64                 # démonstration
#   sudo ./chroot-demo.sh armhf /bin/bash         # shell dans le rootfs
#   sudo ./chroot-demo.sh ppc64le --umount        # démonter l'overlay
#
# Rootfs : variable ROOTFS, sinon ./rootfs-<cible> à côté de ce script
# (rootfs-ppc64le, rootfs-aarch64, rootfs-armhf, rootfs-riscv64).  Ceux des
# mesures du README : AlmaLinux 10.2 (ppc64le, aarch64 : extraits des images
# cloud) et Ubuntu 26.04 « base » (armhf, riscv64 : archives
# ubuntu-base-26.04-base-<arch>.tar.gz de cdimage.ubuntu.com).
# Variable WORK : dossier de l'overlay (défaut ./.chroot-<cible>).
# Dans le chroot : QEMU_PPC_FPJIT=0, QEMU_ARM_FPJIT=0, QEMU_RISCV_FPJIT=0
# (flottant JIT désactivé, helpers C), QEMU_PAUTH_JIT=0 (PAuth par helpers),
# QEMU_CPU=... (modèle ; riscv64 : max, VLEN 128, ou max,vlen=256).
set -e
HERE=$(dirname "$(readlink -f "$0")")
case "$1" in
ppc64le) qt=ppc64le ;;
aarch64) qt=aarch64 ;;
armhf|arm) qt=arm; set -- armhf "${@:2}" ;;
riscv64) qt=riscv64 ;;
*)
    echo "usage : $0 <ppc64le|aarch64|armhf|riscv64> [--umount | commande...]" >&2
    exit 1
    ;;
esac
arch=$1
shift
ROOTFS=$(readlink -f "${ROOTFS:-$HERE/rootfs-$arch}")
W=${WORK:-$HERE/.chroot-$arch}
M=$W/merged

if [ "$1" = "--umount" ]; then
    for m in $M/dev $M/proc $M; do
        if mountpoint -q $m; then umount $m; fi
    done
    echo "overlay démonté ($W/upper conserve les modifications)"
    exit 0
fi

[ -d "$ROOTFS/usr" ] || {
    echo "rootfs $arch introuvable : $ROOTFS (variable ROOTFS)" >&2
    exit 1
}

# (ré)enregistre bin/qemu-<cible> pour les ELF de cette architecture
"$HERE/install-binfmt.sh" $qt > /dev/null

mkdir -p $W/upper $W/work $M
mountpoint -q $M || mount -t overlay overlay \
    -o lowerdir=$ROOTFS,upperdir=$W/upper,workdir=$W/work $M
mountpoint -q $M/proc || mount -t proc proc $M/proc
mountpoint -q $M/dev || mount --bind /dev $M/dev
cp -f /etc/resolv.conf $M/etc/resolv.conf 2>/dev/null || true

if [ $# -gt 0 ]; then
    exec chroot $M "$@"
fi

echo "== binfmt_misc (côté hôte)"
sed -n '1,3p' /proc/sys/fs/binfmt_misc/qemu-$qt
echo
echo "== un processus du chroot, vu depuis l'hôte"
chroot $M /bin/sleep 2 &
sleep 0.5
echo "PID $! : /proc/$!/exe -> $(readlink /proc/$!/exe)"
wait
echo
echo "== dans le chroot (chaque commande est un execve $arch)"
chroot $M /bin/bash -c '
  . /etc/os-release; echo "système      : $PRETTY_NAME"
  echo "uname -m     : $(uname -m)"
  c=$(grep -m1 -E "^(cpu|model name|Processor|isa)" /proc/cpuinfo | cut -d: -f2)
  [ -n "$c" ] && echo "cpu          :$c"
  LD_SHOW_AUXV=1 /bin/true | grep -E "^AT_HWCAP2?:" | sed "s/^/auxv         : /"
  h=$(ld.so --list-diagnostics 2>/dev/null | grep -o "dl_hwcaps_subdirs=.*" | grep -v "=\"\"")
  [ -n "$h" ] && echo "glibc-hwcaps : $h"
  python3 -c "import math, sys; print(\"python\", sys.version.split()[0], \": sum(sin(i)*sqrt(i)) =\", sum(math.sin(i)*math.sqrt(i) for i in range(200000)))"
  seq 1 200000 | awk "{s += sqrt(\$1)} END {printf \"awk          : %.6f\n\", s}"
  head -c 20000000 /dev/zero | sha256sum | sed "s/^/sha256sum    : /"
  if command -v rpm > /dev/null; then
    rpm -q glibc python3 | sed "s/^/rpm -q       : /"
  else
    dpkg-query -W -f "\${Package} \${Version}\n" libc6 python3 | sed "s/^/dpkg-query   : /"
  fi
'
