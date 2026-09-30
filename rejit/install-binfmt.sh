#!/bin/bash
# Enregistre les qemu-user JIT de bin/ comme interpréteurs binfmt_misc des
# exécutables ELF ppc64le, aarch64, armv7 (arm 32 bits) et riscv64 : ensuite,
# tout execve() d'un tel binaire (y compris dans un chroot) passe
# automatiquement par eux.
#
#   sudo ./install-binfmt.sh                      # les quatre
#   sudo ./install-binfmt.sh aarch64 arm          # seulement ceux-là
#   sudo ./install-binfmt.sh --remove [cibles]    # retire les enregistrements
#   QEMU=/chemin/qemu sudo ./install-binfmt.sh aarch64   # un autre binaire
#
# Cibles : ppc64le, aarch64, arm, riscv64.  L'enregistrement ne survit pas à
# un redémarrage : relancer le script.  Le noyau garde ouvert le binaire
# enregistré (drapeau F) : après l'avoir remplacé, relancer le script.
# Drapeaux : F (le noyau ouvre l'interpréteur tout de suite, indispensable
# pour un chroot où son chemin n'existe pas), P (argv[0] préservé),
# O (le binaire est passé ouvert à QEMU).
set -e
HERE=$(dirname "$(readlink -f "$0")")
B=/proc/sys/fs/binfmt_misc

remove=0
if [ "$1" = "--remove" ]; then
    remove=1
    shift
fi
targets=${*:-ppc64le aarch64 arm riscv64}

[ -e $B/register ] || mount -t binfmt_misc binfmt_misc $B

for t in $targets; do
    # Les \x sont décodés par le noyau (echo sans -e).
    case $t in
    ppc64le)
        # ELF 64 bits little-endian, ET_EXEC ou ET_DYN, e_machine 21 (EM_PPC64)
        magic='\x7f\x45\x4c\x46\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x15\x00'
        mask='\xff\xff\xff\xff\xff\xff\xff\xfc\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\x00'
        ;;
    aarch64)
        # ELF 64 bits little-endian, ET_EXEC ou ET_DYN, e_machine 183 (EM_AARCH64)
        magic='\x7f\x45\x4c\x46\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xb7\x00'
        mask='\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff'
        ;;
    arm)
        # ELF 32 bits little-endian, ET_EXEC ou ET_DYN, e_machine 40 (EM_ARM)
        magic='\x7f\x45\x4c\x46\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x28\x00'
        mask='\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff'
        ;;
    riscv64)
        # ELF 64 bits little-endian, ET_EXEC ou ET_DYN, e_machine 243 (EM_RISCV)
        magic='\x7f\x45\x4c\x46\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xf3\x00'
        mask='\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff'
        ;;
    *)
        echo "cible inconnue : $t (ppc64le, aarch64, arm ou riscv64)" >&2
        exit 1
        ;;
    esac
    name=qemu-$t
    # Retirer un ancien enregistrement (celui d'un paquet qemu-user-static inclus)
    [ -e $B/$name ] && echo -1 > $B/$name
    if [ $remove = 1 ]; then
        echo "binfmt $name retiré"
        continue
    fi
    qemu=$(readlink -f "${QEMU:-$HERE/bin/qemu-$t}")
    [ -x "$qemu" ] || { echo "introuvable : $qemu" >&2; exit 1; }
    echo ":$name:M::$magic:$mask:$qemu:POF" > $B/register
    echo "== $name"
    sed -n '1,3p' $B/$name
done
