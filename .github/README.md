# QEMU ReJit

**Fork de QEMU 11.1.1** qui accélère l'émulation utilisateur (linux-user) de quatre architectures sur hôte x86-64 avec AVX-512 :

| Émulateur | Invité | Gain typique (charges réelles dans un chroot) |
|---|---|---|
| `qemu-ppc64le` | IBM Power11 | ×1,2 à ×3,9 |
| `qemu-aarch64` | ARM 64 bits | ×1,3 à ×3,6 |
| `qemu-arm` | ARMv7 32 bits (armhf) | ×1,1 à ×2,1 |
| `qemu-riscv64` | RISC-V 64 bits RVA23, vecteurs RVV 1.0 | ×1,7 à ×9,9 |

Le flottant, le SIMD, les vecteurs RVV, SHA-256 et l'authentification de pointeurs d'ARM passaient par des fonctions C (softfloat, un appel par instruction). Ils sont maintenant compilés par le JIT de QEMU (TCG) en code AVX-512, avec la sémantique exacte de chaque architecture : NaN, drapeaux d'exception, arrondis. Les tests différentiels contre le QEMU d'origine donnent des sorties identiques bit à bit.

Le reste de QEMU n'est pas modifié : **toutes les cibles** (29 émulateurs système, 33 émulateurs utilisateur, dont mips, x86, s390x…) se compilent avec le `configure` habituel et se comportent comme avec QEMU 11.1.1 (suite `check-tcg` de QEMU passée sur 15 architectures).

## Démarrer

Binaires statiques prêts à l'emploi pour Linux x86-64 : [`rejit/bin/`](../rejit/bin/).

```sh
git clone https://github.com/ytrezq/qemu-user-ReJit-Itanium.git
cd qemu-user-ReJit-Itanium/rejit
sudo ./install-binfmt.sh                        # binfmt_misc : les ELF ppc64le, aarch64, arm, riscv64 passent par rejit/bin
sudo ROOTFS=/chemin/rootfs ./chroot-demo.sh riscv64   # démonstration dans un rootfs
```

Compiler (comme QEMU) :

```sh
mkdir build && cd build
../configure        # toutes les cibles
make -j"$(nproc)"
```

## Documentation

- [`rejit/README.md`](../rejit/README.md) : ce qui a changé, résultats, validation, écarts connus, portée (mode système, autres cibles, autres hôtes) ;
- [`rejit/README-ppc.md`](../rejit/README-ppc.md) : le détail PowerPC ;
- [`README.rst`](../README.rst) : le README de QEMU ;
- les commits du fork : `git log --oneline c3d48b7d1e..` (`c3d48b7d1e` : QEMU 11.1.1).

Licence : celle de QEMU (GPL-2.0, voir [`LICENSE`](../LICENSE)).
