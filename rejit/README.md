# QEMU ReJit : qemu-user accéléré pour Power11, AArch64, ARMv7 et RISC-V 64

Ce dépôt est un **fork de QEMU 11.1.1** (historique amont complet jusqu'au tag `v11.1.1`, puis les commits du fork). Il accélère, sur hôte x86-64 avec AVX-512, l'émulation utilisateur (linux-user) de quatre architectures :

- `qemu-ppc64le`, pour IBM Power11 ;
- `qemu-aarch64`, pour ARM 64 bits ;
- `qemu-arm`, pour ARMv7 32 bits (armhf) ;
- `qemu-riscv64`, pour RISC-V 64 bits (profil RVA23, vecteurs RVV 1.0 compris).

Le reste de QEMU n'est pas modifié et se compile comme d'habitude : **toutes les cibles** (29 émulateurs système, 33 émulateurs utilisateur, dont mips, x86, s390x…) se construisent avec le `configure` de QEMU et s'exécutent comme avec le QEMU d'origine (section « Le fork complet »).

QEMU (TCG) est déjà un JIT : il traduit le code invité en code x86 par blocs. Mais le flottant, plusieurs instructions vectorielles et cryptographiques, et l'authentification de pointeurs d'ARM y passaient par des **fonctions C**. Il y avait un appel par instruction, et souvent un émulateur logiciel (softfloat) derrière. Sur RISC-V, c'était aussi le cas de presque tout le jeu vectoriel RVV et de chaque `vsetvli`.

Ces appels sont remplacés par du **code généré**. Pour cela, TCG a reçu de nouvelles opérations que le backend x86-64 émet en AVX-512, et les quatre front-ends s'en servent. La sémantique de chaque architecture est conservée : NaN, drapeaux d'exception (FPSCR, FPSR, `fflags`), arrondis, saturation des conversions, éléments vectoriels hors de `vl`. Les tests différentiels contre le QEMU d'origine donnent des sorties identiques, bit à bit.

## Contenu du dépôt

| Chemin | Rôle |
|---|---|
| l'arbre de QEMU | QEMU 11.1.1 et les commits du fork : `git log --oneline c3d48b7d1e..` (ou `git format-patch c3d48b7d1e..` pour la série de patches) ; `c3d48b7d1e` est le commit du tag `v11.1.1` de QEMU |
| `rejit/bin/qemu-ppc64le`, `qemu-aarch64`, `qemu-arm`, `qemu-riscv64` | binaires statiques pour Linux x86-64 (strippés), compilés depuis ce dépôt |
| `rejit/install-binfmt.sh` | enregistre ces binaires dans binfmt_misc (flags `POF`) ; à relancer après un redémarrage ou le remplacement d'un binaire |
| `rejit/chroot-demo.sh` | chroot dans le rootfs d'une cible via un overlay jetable ; démonstration ou shell |
| `rejit/tests/ppc/`, `rejit/tests/arm/`, `rejit/tests/riscv/` | tests différentiels et leurs scripts `run-tests.sh` |
| `rejit/bench/` | charges (`work.sh`), comparaison A/B (`compare.sh`), micro-benchmarks, résultats bruts |
| `rejit/README-ppc.md` | détail du travail PowerPC |

## Compiler

Comme QEMU :

```sh
git clone https://github.com/ytrezq/qemu-user-ReJit-Itanium.git
cd qemu-user-ReJit-Itanium
mkdir build && cd build
../configure            # toutes les cibles, système et utilisateur
make -j"$(nproc)"
```

`configure` télécharge les sous-projets meson qui manquent (keycodemapdb, berkeley-softfloat-3…), comme pour tout dépôt git de QEMU.

Le dépôt ne porte pas le tag `v11.1.1` de QEMU ; sans lui, `qemu-* --version` affiche simplement `11.1.1`. Pour retrouver le suffixe `v11.1.1-<n>-g<commit>` : `git fetch https://gitlab.com/qemu-project/qemu.git tag v11.1.1`.

Les binaires de `rejit/bin` ont été produits ainsi :

```sh
../configure --target-list=ppc64le-linux-user,aarch64-linux-user,arm-linux-user,riscv64-linux-user \
  --static --disable-system --enable-linux-user --without-default-features \
  --enable-capstone --disable-docs --disable-tools
make -j"$(nproc)" qemu-ppc64le qemu-aarch64 qemu-arm qemu-riscv64
strip qemu-ppc64le qemu-aarch64 qemu-arm qemu-riscv64
```

`--enable-capstone` (désassembleur pour `-d in_asm`) demande `libcapstone-dev` ; on peut l'omettre.

## Binaires

Les quatre binaires de `rejit/bin` sont statiques et tournent sur tout Linux x86-64. Le code généré accéléré demande AVX-512 F, BW, DQ et VL (Skylake-SP, Ice Lake, Sapphire Rapids, Zen 4 et suivants) ; VBMI pour les permutations d'octets et SHA pour SHA-256. **Sans ces extensions, rien ne casse** : les instructions concernées passent par les helpers C d'origine, avec les mêmes résultats que QEMU.

`qemu-riscv64 --version` affiche le commit dont ils sont issus (`v11.1.1-…-g<commit>`).

## Utilisation

```sh
cd rejit
sudo ./install-binfmt.sh                   # les quatre (ou : ./install-binfmt.sh aarch64 riscv64)
sudo ROOTFS=/chemin/rootfs-riscv64 ./chroot-demo.sh riscv64       # démonstration
sudo ROOTFS=/chemin/rootfs-armhf ./chroot-demo.sh armhf /bin/bash # shell Ubuntu armv7
sudo ./chroot-demo.sh ppc64le --umount     # démonter l'overlay
sudo ./install-binfmt.sh --remove          # retirer les enregistrements
```

Sans `ROOTFS`, `chroot-demo.sh` cherche `rejit/rootfs-<cible>`. Les rootfs des mesures ci-dessous :

| Cible | Rootfs |
|---|---|
| ppc64le | AlmaLinux 10.2, extrait de l'image cloud officielle |
| aarch64 | AlmaLinux 10.2, extrait de l'image cloud officielle |
| armhf | Ubuntu 26.04 « base » armhf (`ubuntu-base-26.04-base-armhf.tar.gz`) |
| riscv64 | Ubuntu 26.04 « base » riscv64 (`ubuntu-base-26.04-base-riscv64.tar.gz`) |

AlmaLinux ne publie pas de version ARMv7, d'où Ubuntu pour armhf. Ubuntu 26.04 riscv64 est compilée pour le profil RVA23 : elle suppose les vecteurs RVV et les extensions de bits (Zba, Zbb, Zbs). GCC y vectorise beaucoup de boucles, jusqu'aux recherches dans les chaînes (`vle8ff.v` apparaît 791 fois dans `libc.so.6`). C'est donc un bon terrain pour le JIT vectoriel. Le processeur émulé par défaut (`max`) fournit ce profil.

Dans le chroot, chaque `execve()` d'un binaire de la cible passe par le noyau (binfmt_misc), qui lance le `rejit/bin/qemu-*` correspondant. Vu de l'hôte, `/proc/<pid>/exe` d'un processus du chroot pointe sur ce binaire. La démonstration affiche aussi le système, `uname -m`, les HWCAP (sur riscv64, la chaîne `isa` de `/proc/cpuinfo`), un calcul Python et un calcul awk, un `sha256sum` et les paquets installés.

### Variables d'environnement

Elles se passent dans le chroot, ou directement à `qemu-*` :

| Variable | Effet |
|---|---|
| `QEMU_PPC_FPJIT=0` | flottant PowerPC par les helpers C d'origine |
| `QEMU_ARM_FPJIT=0` | flottant ARM (A64 et A32) par les helpers C d'origine |
| `QEMU_RISCV_FPJIT=0` | flottant RISC-V (F et D scalaires, RVV) par les helpers C d'origine |
| `QEMU_PAUTH_JIT=2` | défaut : cache de PAC dans le code généré |
| `QEMU_PAUTH_JIT=1` | hachage IMPDEF entièrement en ligne |
| `QEMU_PAUTH_JIT=0` | helpers C d'origine |
| `QEMU_CPU=…` | modèle émulé (défaut : `power11` pour ppc64le, `max` pour ARM et RISC-V) ; sur RISC-V, `max,vlen=256` donne des vecteurs de 256 bits au lieu de 128 |

Ces variables servent surtout à comparer.

## Résultats

Hôte : Xeon à 2,1 GHz (2 cœurs), AVX-512 avec FP16, SHA-NI et GFNI (Sapphire Rapids). Référence : QEMU 11.1.1 non modifié, compilé de la même façon. Mesures faites avec `bench/compare.sh`, pour les quatre cibles le même jour :

- dans le chroot, via binfmt ;
- exécutions alternées référence/JIT, 3 de chaque, temps minimum ;
- toutes les sorties sont identiques entre les deux QEMU.

Résultats bruts : `bench/results-*.txt`. Chaque case donne origine → JIT.

| Charge | aarch64 | armhf | ppc64le | riscv64 |
|---|---:|---:|---:|---:|
| `python3` json/re | 5,04 → 2,40 s (×2,1) | 2,84 → 2,00 s (×1,4) | 3,50 → 2,01 s (×1,7) | 14,93 → 1,50 s (×9,9) |
| `python3` flottant | 3,13 → 1,38 s (×2,3) | 2,06 → 1,15 s (×1,8) | 2,66 → 1,16 s (×2,3) | 6,57 → 0,75 s (×8,8) |
| `gzip -6` | 3,48 → 1,95 s (×1,8) | 1,17 → 1,01 s (×1,2) | 2,38 → 1,98 s (×1,2) | 1,83 → 0,95 s (×1,9) |
| `xz -2` | 3,22 → 1,90 s (×1,7) | 2,79 → 2,18 s (×1,3) | 2,17 → 1,52 s (×1,4) | 8,48 → 1,38 s (×6,1) |
| `sort`, `sort -n` | 0,53 → 0,24 s (×2,2) | 0,53 → 0,38 s (×1,4) | 0,45 → 0,22 s (×2,1) | 1,53 → 0,35 s (×4,4) |
| `sha256sum` + `md5sum` | 0,49 → 0,19 s (×2,6) | 0,42 → 0,33 s (×1,3) | 0,96 → 0,24 s (×3,9) | 1,59 → 0,41 s (×3,9) |
| `awk` (sqrt, sin) | 5,50 → 1,52 s (×3,6) | 3,89 → 1,81 s (×2,1) | 4,27 → 1,29 s (×3,3) | 6,39 → 1,22 s (×5,2) |
| `grep` | 0,23 → 0,18 s (×1,3) | 0,22 → 0,18 s (×1,2) | 0,22 → 0,17 s (×1,3) | 0,24 → 0,13 s (×1,8) |
| `rpm -qa` (Ubuntu : `dpkg-query`) | 4,63 → 2,25 s (×2,1) | 0,29 → 0,26 s (×1,1) | 5,68 → 2,20 s (×2,6) | 0,37 → 0,22 s (×1,7) |
| `dnf --version` (Ubuntu : `apt-cache`) | 2,27 → 1,52 s (×1,5) | 1,16 → 0,89 s (×1,3) | 1,93 → 1,30 s (×1,5) | 1,64 → 0,81 s (×2,0) |

Le gain dépend de la part du flottant, du SIMD et de PAuth dans chaque charge :

- **armhf** : le code entier était déjà traduit directement par QEMU. Seul le flottant y passait par des helpers, d'où des gains plus faibles hors `awk` et Python.
- **riscv64** : les gains sont les plus grands. Dans le QEMU d'origine, chaque `vsetvli` et chaque instruction vectorielle appelaient un helper. Or glibc, zlib et CPython, compilés pour RVA23, en exécutent sans cesse : copies, recherches dans les chaînes, `slide_hash` de zlib… De plus, dès qu'un processus avait plusieurs threads, chaque accès mémoire portait une barrière.
- **Programmes courts** (`grep`, `dpkg`, `dnf`) : le temps restant est surtout celui de la traduction TCG, déjà réduite par les changements communs (section 3 plus bas, et « Pistes »).

### Micro-benchmarks

Source : `bench/*/bench.c`, compilé pour chaque cible ; x86 natif pour comparaison. Détails : `bench/results-micro.txt`.

| Noyau | aarch64 | armv7 | ppc64le | riscv64 | x86 natif |
|---|---:|---:|---:|---:|---:|
| entier | 0,78 → 0,74 s | 2,60 → 2,63 s | 0,59 → 0,57 s | 0,49 → 0,49 s | 0,50 s |
| flottant | 1,76 → **0,34 s** | 1,74 → **0,40 s** | 6,28 → **0,28 s** | 1,83 → **0,21 s** | 0,13 s |
| vecteur (`a*b+c*0.5f`) | 1,29 → **0,14 s** | 1,19 → **0,44 s** | 3,42 → **0,15 s** | 3,79 → **0,10 s** | 0,02 s |
| chaînes (strlen, strchr, memmove) | 1,96 → **0,27 s** | 2,26 → **0,76 s** | 0,39 → 0,37 s | 0,23 → 0,23 s | 0,01 s |

Sur riscv64, `strlen` de glibc est du code scalaire Zbb (`orc.b`), que les deux QEMU traduisent de la même façon, d'où l'absence de gain sur « chaînes ». Le code vectorisé de glibc (`strrchr`, `rawmemchr`… en `vle8ff.v`) apparaît dans `strbench.c` ci-dessous.

Benchmarks ciblés :

| Test | Origine | JIT | Gain |
|---|---:|---:|---:|
| aarch64 : 100 M d'appels de fonction avec PACIASP/AUTIASP (`bench/arm/pacbench.c`) | 8,97 s | 1,26 s | ×7,1 |
| aarch64 : boucles vectorisées sqrtf, float→int, floor, int→float (`vecfp.c`) | 8,01 s | 0,90 s | ×8,9 |
| aarch64 : produit de matrices, FMLA par élément (`matmul.c`) | 2,97 s | 0,34 s | ×8,7 |
| aarch64 : fonctions chaînes de glibc (`strbench.c`) | 4,44 s | 1,22 s | ×3,6 |
| armv7 : fonctions chaînes de glibc (`strbench.c`) | 5,08 s | 4,25 s | ×1,2 |
| ppc64le : fonctions chaînes de glibc | 1,22 s | 0,70 s | ×1,7 |
| ppc64le : MMA `xvf32gerpp` (`tests/ppc/mmabench.c`) | 2,41 s | 0,12 s | ×20 |
| riscv64 : fonctions chaînes de glibc, RVV (`bench/riscv/strbench.c`) | 13,65 s | 0,73 s | ×19 |

## Ce qui a été modifié

Chaque commit touche soit le cœur commun (`accel/tcg`, `tcg`, backend `tcg/x86_64`), soit un front-end (`target/ppc` ; `target/arm`, qui contient A64 et A32 ; `target/riscv`). Tout ce qui est commun profite aux quatre cibles, et en partie aux autres (section « Le fork complet »).

### 1. Commun : branchements indirects

- **Jump cache sondé dans le code généré** (mode utilisateur). C'est le cas des retours de fonction et des appels indirects : `blr`/`bctr` sur PowerPC, `RET`/`BR`/`BLR` sur A64, `BX lr`/`POP {pc}` sur A32, `jalr` sur RISC-V. Le code généré compare la clé du bloc (pc, flags, `cs_base`) au lieu d'appeler `helper_lookup_tb_ptr`.
- **Jump cache plus gros et mieux haché.**
  - Le cache passe à 16 K entrées.
  - En mode utilisateur, le hachage convient au code aligné sur 2 octets (Thumb, RISC-V compressé) comme sur 4.
  - `cs_base` entre dans le hachage. Chez ARM, `cs_base` contient BTYPE et l'état IT : une fonction appelée par `BL` et par `BLR` n'évince plus sa propre entrée (awk aarch64 −15 %).
- **Chaînage direct entre pages** (`goto_tb`) en mode utilisateur. C'est sûr, car les blocs sont invalidés quand les droits d'une page changent.
- **Clés de blocs génériques** (`TCGCPUOps.tb_flags_generic_mask`). Un front-end peut déclarer qu'un bloc ne dépend pas de certains flags. Le bloc est alors retrouvé quelle que soit leur valeur. Seul RISC-V s'en sert (section 7), en mode utilisateur ; pour les autres cibles, rien ne change.

### 2. Commun : flottant et SIMD en AVX-512

- **Nouvelles opérations TCG** (`include/tcg/tcg-fpop.h`, `tcg-crypto.h`) :
  - `fpop1/2/3_vec` : arithmétique, FMA, max/min IEEE 754-2008 et 2019, racine, arrondis, conversions ;
  - `fpop1/2/3_i64` : les mêmes, en scalaire, sur des temporaires 64 bits (RISC-V garde ses registres flottants dans des globales i64) ;
  - `fpcmpcc_vec` : produit un champ CR PowerPC ou NZCV ARM ;
  - `cmpmask_vec` : comparaison entière vers un masque de bits (`vpcmp` puis `kmovq`) ; `anytrue_vec` (`vptest`) ;
  - `perm2b_vec`, `ld32/st32_vec` ;
  - `ldm/stm_vec` : accès masqués, par octet ou par élément, masque constant ou non (un masque plein devient un `vmovdqu` ordinaire) ; `ldmm_vec` : chargement masqué qui garde les éléments inactifs ;
  - `crypto_vec` : étapes SHA-256.
- **Émission par le backend x86-64** (`tcg/x86_64/tcg-target-fpop.c.inc`, EVEX) :
  - registres de masque `k1`–`k3` ;
  - `xmm16`–`xmm23` comme temporaires ;
  - constantes diffusées `{1toN}`.
  - Les autres backends (et x86-64 sans AVX-512) ne déclarent pas ces opérations : `tcg_can_emit_fpop()` et ses semblables répondent non, et les front-ends gardent leurs helpers.
- **Correctifs sans branchement du chemin rapide**, selon la cible :
  - **NaN** : priorités A, C, B sur PowerPC. Sur ARM, les NaN signalants passent d'abord, puis l'ordre c, a, b du FMA, puis le NaN par défaut. Sur RISC-V, tout résultat NaN est le NaN canonique. `0×∞+qNaN` est invalide sur ARM et RISC-V.
  - **« Petitesse » avant arrondi** (underflow) pour PowerPC et ARM ; RISC-V la teste après arrondi, comme x86.
  - **min/max** : `minimumNumber`/`maximumNumber` d'IEEE 754-2019 pour RISC-V (`vrange` et masques de classe).
  - **Conversions** saturantes : un NaN donne 0 sur ARM, la valeur maximale sur RISC-V.
- **Drapeaux d'exception.** Ils s'accumulent dans MXCSR et sont relus paresseusement, quand l'invité lit FPSCR (PowerPC), FPSR/FPSCR (ARM) ou `fflags`/`fcsr` (RISC-V). Les chemins de code C de l'hôte sauvegardent et restaurent MXCSR : syscalls, traduction, signaux. MXCSR appartient au thread hôte : c'est pourquoi ce flottant en code généré est réservé au mode utilisateur, où chaque CPU émulé est un thread et où tout lecteur de ses registres tourne sur ce thread.
- **SHA-256 avec SHA-NI.** `SHA256H/H2/SU0/SU1` d'A64 sont traduits en `SHA256RNDS2`/`MSG1`/`MSG2`.
- **Optimiseur** : un `stm_vec` range par un pointeur qui peut viser `env`. L'optimiseur oublie donc les copies mémoire qu'il suivait ; sinon, un registre vectoriel relu juste après aurait gardé son ancienne valeur.

### 3. Commun : traduction plus rapide

Les programmes courts passent l'essentiel de leur temps à traduire. Ces changements valent pour toutes les cibles.

- **Analyse de vivacité par listes** (`liveness_pass_1`). Elle ne parcourt plus tous les temporaires du bloc à chaque fin de bloc, appel ou accès mémoire : seulement ceux qui sont concernés. Le résultat est identique, vérifié sur des dizaines de milliers de blocs. Sa part dans `dnf` passe de 8,5 % à 4 %.
- **Allocation de registres en fin de bloc.** Elle ne sauve plus que les temporaires `TEMP_TB`.
- **Accès mémoire sans chemin lent.** En mode utilisateur, un `qemu_ld`/`qemu_st` sans test d'alignement n'appelle jamais de helper. Il ne vide donc plus les registres « call-clobbered ».
- **Accès sous-alignés en ligne.** `LDRD`/`STRD` et `VLDR`/`VSTR` à une adresse alignée sur 4 (atomicité `SUBALIGN`) ne prennent plus le chemin lent.
- **Table des constantes.** `tcg_constant_*` cherchait ses temporaires dans une table GLib par type. Vidée à chaque bloc, elle rétrécissait, puis grandissait de nouveau avec les constantes du bloc suivant (rehachages).
  - Elle est remplacée par une table à adressage ouvert avec un numéro de génération : tout oublier coûte une incrémentation.
  - Gain mesuré sur riscv64 : 5 à 10 % sur les charges dominées par la traduction (`sort` −10 %, `apt-cache` −8 %, Python −6 %).

### 4. AArch64

- **Flottant scalaire (S, D) en code généré**
  - Arithmétique : `FADD` `FSUB` `FMUL` `FDIV` `FNMUL` `FABD` `FMAX` `FMIN` `FMAXNM` `FMINNM`.
  - FMA : `FMADD` `FMSUB` `FNMADD` `FNMSUB`.
  - Racine et arrondis : `FSQRT` `FRINTN/P/M/Z/I/X`.
  - Comparaisons : `FCMP`/`FCMPE` (NZCV), `FCMEQ/GE/GT`, `FACGE/GT`.
  - Conversions : `FCVT` S↔D, `SCVTF`/`UCVTF`, `FCVTZS`/`FCVTZU` vers registre général.
- **AdvSIMD 4S/2D en code généré**
  - Les mêmes opérations arithmétiques, plus `FMLA`/`FMLS` et les comparaisons.
  - `FSQRT`, `FRINT*`, `SCVTF`, `UCVTF`, `FCVTZS`, `FCVTZU`.
  - `FMUL`/`FMLA`/`FMLS` **par élément** : l'élément est diffusé depuis la mémoire.
- **FPSR et FPCR**
  - Les drapeaux cumulatifs IOC, DZC, OFC, UFC, IXC de FPSR correspondent un pour un à ceux de MXCSR.
  - Un FPCR non standard fait repasser le flottant par les helpers exacts, via un bit `FPSOFT` des flags des blocs : `FZ`, `DN`, arrondi dirigé, `AH`, `FIZ`, `NEP`.
- **Authentification de pointeurs (PAuth)**, en mode utilisateur
  - Glibc et toute la distribution sont compilées avec `-mbranch-protection` : il y a un `PACIASP`/`AUTIASP` dans chaque fonction non feuille.
  - Le code généré consulte un cache (pointeur, modificateur, pointeur signé) avec une seule comparaison. Le `AUTIASP` d'un retour retrouve le `PACIASP` de l'entrée, et une fonction appelée en boucle retrouve l'appel précédent.
  - Un défaut de cache appelle `helper_pac_fill`/`helper_aut_fill`, qui font l'instruction complète (faute FPAC comprise) et remplissent le cache.
  - Cela fonctionne avec tout algorithme de PAC. Le cache est vidé à chaque changement de clés (`PR_PAC_RESET_KEYS`, `execve`).
- **BTI** : le BTYPE d'un `BR` est calculé à la traduction.
- **Opérations par paires** : `ADDP`, `[SU]MAXP`, `[SU]MINP` en deux `vpermi2b` et une opération. Ce sont les `strlen`, `strchr` et `memchr` de glibc.
- **FEAT_MOPS** (`memcpy`/`memset` de glibc en `CPY*`/`SET*`) : en mode utilisateur, le test d'activation ne recalcule plus HCR/HCRX à chaque appel.

### 5. AArch32 (ARMv7)

- **VFP scalaire (F32, F64) en code généré**
  - Arithmétique : `VADD` `VSUB` `VMUL` `VDIV` `VNMUL`.
  - Multiplications-additions non fusionnées, avec deux arrondis comme l'architecture : `VMLA` `VMLS` `VNMLA` `VNMLS`.
  - FMA : `VFMA` `VFMS` `VFNMA` `VFNMS`.
  - Autres : `VMAXNM`/`VMINNM`, `VSQRT`, `VCMP`/`VCMPE`.
  - Conversions : `VCVT` F32↔F64 et entier↔flottant.
  - FPSCR est tiré des drapeaux de l'hôte, comme FPSR.
- **SIMD entier des ARMv6/v7**
  - `SADD8/16`, `SSUB8/16`, `UADD8/16`, `USUB8/16` avec leurs bits GE ;
  - `SEL` ;
  - les écritures de NZCV, en opérations TCG au lieu de helpers.
- **Opérations par paires** : `VPADD`, `VPMAX`, `VPMIN`.
- **Jump cache sondé en ligne**, avec l'état Thumb et IT.

### 6. PowerPC

- **Flottant classique, VSX scalaire et vectoriel, MMA (`xvf32ger*`, `xvf64ger*`)** en AVX-512.
- **FPSCR paresseux.**
- **Altivec** : `vshasigma`, `vperm`/`xxperm` en un `vpermi2b`.
- **`lxvl`/`stxvl`** en accès masqués.
- **Processeur par défaut** : Power11.

Le détail est dans [README-ppc.md](README-ppc.md).

### 7. RISC-V 64 (RVA23)

- **Barrières RVTSO.** Le processeur `max` a l'extension Ztso (ordre mémoire TSO). Dès qu'un processus avait plusieurs threads (Python 3.14, xz), le QEMU d'origine mettait alors une barrière complète à chaque chargement et à chaque rangement, soit une instruction verrouillée par accès sur x86. RVTSO ne demande que les ordres que x86 garantit déjà. Il n'y a donc plus de barrière pour les accès ordinaires, les accès vectoriels, ni pour `lr` sans `.aq`.
- **`vsetvli`/`vsetivli`**
  - Avant, chaque tour de boucle vectorielle payait un appel à `helper_vsetvl` et une recherche de bloc. Maintenant, `vl` est calculé dans le code généré, et le bloc est chaîné par `goto_tb`, avec une sortie pour `vl = VLMAX` et une pour `vl < VLMAX`.
  - Quand `vl` est connu à la traduction (`vsetivli`, ou `vsetvli` avec `rs1 = x0` et `rd ≠ x0`, qui donne VLMAX), le bloc continue, avec l'état vectoriel connu.
- **Blocs indépendants de l'état vectoriel** (mode utilisateur). Les flags d'un bloc RISC-V contiennent l'état vectoriel (SEW, LMUL, `vl = VLMAX`…), que l'ABI laisse indéfini à travers les appels.
  - Avant : après chaque `memcpy` ou `strlen` vectorisé, le code scalaire de l'appelant était retraduit, et ses versions s'évinçaient l'une l'autre du jump cache. Une boucle Python de 600 000 tours faisait 6,3 millions de recherches de bloc, dont 99,6 % d'échecs.
  - Maintenant : un bloc sans instruction vectorielle (hormis `vsetvl*`) a une clé générique, valable pour tout état vectoriel.
- **Chargements et rangements vectoriels** `vle<eew>.v`/`vse<eew>.v`, sans segments, depuis `vstart = 0`, pour VLEN 128 ou 256 :
  - Un accès masqué par registre du groupe : `vmovdqu8/16/32/64`, avec un masque AVX-512 des éléments sous `vl` (et, s'il y a lieu, actifs dans `v0`). Les autres éléments ne sont ni lus ni écrits : ils ne peuvent pas faire de faute, et restent inchangés (« tail/mask undisturbed »).
  - Avec un `vl` connu et sans masque, ce sont des accès ordinaires : 8, 4, 2 et 1 octets, ou un `vmovdqu` entier. En effet, un rangement masqué ne se transmet pas au chargement qui le suit (store forwarding), et GCC copie les petites structures en `vsetivli` + `vle8.v` + `vse8.v`.
  - **`vle<eew>ff.v`** (fault-only-first : les boucles de recherche que GCC vectorise, comme `strrchr` ou `rawmemchr` de glibc). Si tous les éléments sous `vl` sont dans la page du premier, `vl` ne peut pas changer : le chargement se fait en ligne et le bloc continue. Sinon, le helper d'origine, qui peut réduire `vl`.
- **Flottant scalaire F et D** en AVX-512
  - Instructions : arithmétique, FMA, racine, `fmin`/`fmax`, comparaisons, conversions.
  - Arrondi : au plus proche (`rm` RNE, ou DYN avec `frm` RNE ; `frm` fait partie de la clé des blocs), ou vers zéro pour les conversions vers entier. Les autres modes passent par les helpers.
  - Le NaN-boxing des valeurs simple précision est vérifié à la lecture et produit à l'écriture.
- **Flottant vectoriel RVV** : `vfadd`, `vfsub`, `vfrsub`, `vfmul`, `vfdiv`, `vfrdiv`, `vfmin`, `vfmax` (`.vv` et `.vf`), `vfsqrt.v` et les huit FMA (`vfmacc`… `vfnmsub`). Conditions : SEW 32 ou 64, `vl = VLMAX`, LMUL ≥ 1, sans masque, `frm` RNE.
- **Entiers RVV**
  - `vsaddu`, `vsadd`, `vssubu`, `vssub` (`.vv`, `.vx`, `.vi`) avec `vxsat`. Le helper faisait un appel indirect par élément : c'était 10 % de gzip (`slide_hash` de zlib).
  - `vmv.v.x`, `vmv.v.i` et `vid.v`, aussi quand `vl < VLMAX` ; `vid.v` aussi masqué.
  - Comparaisons `vms*` vers un masque (un `vpcmp` par registre), opérations `vm*.mm`, `vcpop.m`, `vfirst.m`. Les helpers allaient bit par bit : 4 % de Python, par les fonctions de chaînes de glibc.
- **Lectures de CSR sans sortir du bloc** (mode utilisateur) : `frm`, `fflags`, `fcsr`, `vl`, `vtype`, `vlenb`. Les `sin`, `cos`, `log`… de glibc lisent `frm` à chaque appel.
- **`fflags`** : les drapeaux s'accumulent dans MXCSR. Ils sont relus quand l'invité lit `fflags` ou `fcsr`, et reportés avant chaque syscall.

## Le fork complet : autres cibles, mode système, autres hôtes

Le but est que ce dépôt reste un QEMU complet, qui se compile et se comporte comme si QEMU avait intégré ces changements :

| Contexte | Ce qui change |
|---|---|
| `qemu-ppc64le`, `qemu-aarch64`, `qemu-arm`, `qemu-riscv64` sur x86-64 AVX-512 | tout ce qui précède |
| les mêmes front-ends en gros-boutiste ou 32 bits : `qemu-ppc64`, `qemu-ppc`, `qemu-aarch64_be`, `qemu-armeb` | idem ; les chemins qui dépendent de l'ordre des octets en tiennent compte (vérifié pour `qemu-ppc64` et `qemu-ppc` ; pas de chaîne de compilation ici pour tester `aarch64_be` et `armeb`) |
| `qemu-riscv32` | changements communs, clés de blocs génériques, lectures de CSR sans sortir du bloc, additions saturantes RVV ; le reste du travail RISC-V (`vsetvli` en ligne, accès vectoriels, comparaisons, flottant en code généré) est réservé à riscv64 |
| les autres émulateurs utilisateur (mips, x86_64, s390x, sparc64, loongarch64…) | seulement les changements communs de TCG (sections 1 et 3) ; leurs front-ends sont ceux de QEMU |
| émulation système (`qemu-system-*`) | les changements communs de traduction (section 3), `vsetvli` en ligne et additions saturantes de RISC-V, SIMD entier d'A32, opérations par paires, SHA-256, `vperm`/`vsldoi`/`vshasigma` de PowerPC. Le flottant garde les helpers d'origine ; la sonde du jump cache en ligne, le chaînage entre pages, les clés génériques et le cache de PAC sont propres au mode utilisateur ; le jump cache garde sa taille d'origine (4 K entrées), car il est vidé à chaque vidage du TLB |
| autres hôtes (aarch64, ppc64, riscv64, s390x, loongarch64, sparc64), TCI, x86-64 sans AVX-512 | les nouvelles opérations TCG n'y sont pas déclarées : les front-ends gardent leurs helpers |

## Validation

### Le JIT contre le QEMU d'origine

Tous les tests comparent le JIT au QEMU 11.1.1 d'origine compilé de la même façon.

- **`tests/riscv/run-tests.sh <ref-riscv64>`** : 26 comparaisons, sorties identiques octet par octet. Les tests vectoriels tournent à VLEN 128, 256 et 512 :
  - `rvv_ldst` et `rvv_ldff` :
    - chaque combinaison EEW/SEW/LMUL valide (78), masquée ou non ;
    - `vl` de 0 à VLMAX+1, connu (`vsetivli`) ou non ;
    - adresses non alignées, fautes et réductions de `vl` en fin de page ;
    - une boucle `strlen` ;
  - `rvsat` (avec `vxsat`), `rvmv`, `rvmask` (2 793 cas) et `vsettest` ;
  - `rvvfp` : 683 cas avec NaN, infinis et dénormaux, et `frm` RDN/RTZ pour exercer les helpers ; résultats et `fflags` ;
  - `rvfp` : 226 instructions F et D × modes d'arrondi, résultats et `fflags` ;
  - `rvfp2` : `fflags` à travers les syscalls, les signaux, les threads et les changements de `frm` ;
  - `smc` (code modifié par `vse8`/`vse32`), `vtbtest` (`jalr` après un `vsetivli` dans le bloc), `csrtest`.
- **`tests/arm/run-tests.sh <ref-aarch64> <ref-arm>`** : sorties identiques octet par octet.
  - `fptest-a64` : environ 390 000 cas. Il couvre le FP scalaire, les vecteurs 4S/2D/2S, le FMA, la racine, les arrondis, les conversions et les opérations par élément. Il vérifie le résultat, FPSR et NZCV. Il teste aussi les FPCR non standard, les signaux et les threads.
  - `fptest-a32` : environ 140 000 cas VFP, avec FPSCR.
  - `pactest` et `pactest2` : les quatre clés, des pointeurs quelconques, des codes faux (FPAC), `BRAA`/`RETAA`/`LDRAA`, un reset des clés en cours de route. Les trois modes `QEMU_PAUTH_JIT` sont testés.
  - `mopstest` : `CPY*`, `CPYF*`, `SET*`, avec recouvrements dans les deux sens, passages de page et fautes (registres relus dans la trame du signal).
  - `shatest`, `pairtest` (A64 et A32), `simdtest`.
- **`tests/ppc/run-tests.sh <ref>`** : environ 1,5 million de cas FP/VSX/MMA/Altivec, 0 erreur.
- **Suites de tests de CPython dans les chroots**, 43 modules : `test_math`, `test_float`, `test_decimal`, `test_json`, `test_re`, `test_struct`, `test_threading`, `test_subprocess`… Résultats identiques à ceux du QEMU d'origine, ou meilleurs :
  - ppc64le (Python 3.12) : 43/43. Le JIT met 240 s, l'origine 325 s.
  - aarch64 (Python 3.12) : 42/43. `test_signal` échoue aussi avec l'origine. Le JIT met 283 s, l'origine 427 s.
  - armhf (Python 3.14) : 39/43. Les mêmes 4 échecs qu'avec l'origine : `test_audioop` et `test_unicode` n'existent plus en 3.14 ; `test_signal` et `test_subprocess` échouent aussi.
  - riscv64 (Python 3.14) : 40/43, les mêmes 3 échecs qu'avec l'origine : `test_audioop` et `test_unicode` n'existent plus en 3.14, et `test_subprocess` a les mêmes 8 échecs, des cas qui lancent un exécutable introuvable ou donné par un chemin relatif. Le JIT met 230 s, l'origine 1 025 s.
  - Ces suites doivent être lancées avec SIGINT non ignoré. Une commande mise en arrière-plan par un shell non interactif ignore SIGINT ; `_thread.interrupt_main()` n'a alors aucun effet. Dans ce cas, `test_threading` (`test_print_exception_gh_102056`) tourne sans fin et `test_signal` échoue, avec les deux QEMU. C'est le comportement de CPython, pas celui de l'émulateur.
- **Les 10 charges des quatre cibles** donnent des sorties identiques à l'origine.
- **Builds `--enable-debug-tcg`** : toutes les assertions internes de TCG passent sur les tests et les charges. Cette vérification a permis de corriger deux défauts sur ARM (commit « target/arm, tcg: fixes found by the --enable-debug-tcg build »).

### Le fork complet

Sur Ubuntu 24.04 (GCC 13.3), hôte x86-64 AVX-512 :

- **Compilation.** `configure` par défaut : les 62 cibles (29 `qemu-system-*`, 33 `qemu-*`), avec `--enable-werror`. Aussi avec `--enable-tcg-interpreter` (TCI), avec `--enable-debug-tcg`, et en compilation croisée pour hôte aarch64 (backend TCG aarch64).
- **`make check-tcg`**, la suite de tests TCG de QEMU, avec les compilateurs croisés d'Ubuntu :
  - 15 architectures en mode utilisateur (aarch64, alpha, arm, hppa, m68k, mips, mips64el, mipsel, ppc, ppc64, riscv64, s390x, sh4, sparc64, x86_64) et 6 en mode système (aarch64, alpha, arm, riscv64, s390x, x86_64) ;
  - 1 126 tests, dont les variantes avec plugins, et 174 tests du gdbstub (avec `gdb-multiarch`) : aucun échec. Les tests sautés sont ceux que QEMU saute lui-même (pas de sortie de référence, tests manuels, tests marqués cassés en amont) ;
  - build `--enable-debug-tcg` (assertions internes de TCG) : 746 tests, aucun échec ; build TCI : 354 tests, aucun échec.
- **Démarrage de firmwares** (`tests/qtest/boot-serial-test`) : 35 machines de 15 émulateurs système démarrent (SeaBIOS, OpenBIOS, SLOF, U-Boot, skiboot, BIOS s390…).
- **Chemins de repli.** Sans les nouvelles opérations TCG, les front-ends reviennent aux helpers. Le fork compilé en TCI, et compilé pour hôte aarch64 (ses binaires exécutés sous `qemu-aarch64`), passe les tests différentiels de `rejit/tests` à l'identique du QEMU d'origine.
- **PowerPC gros-boutiste et 32 bits** : `qemu-ppc64` (les tests de `tests/ppc` compilés pour ppc64 BE, 1,5 million de lignes) et `qemu-ppc` (`fptest` sur un G4, 418 000 lignes) donnent les sorties du QEMU d'origine.
- **Cibles non modifiées, programmes réels.** Même configuration de compilation, médiane de 5 exécutions, sorties identiques :

| Programme | QEMU 11.1.1 | fork |
|---|---:|---:|
| mips : `bench/riscv/bench.c` / `strbench.c` (portables) | 10,07 s / 3,19 s | 10,05 s / 3,08 s |
| mipsel : les mêmes | 12,50 s / 3,01 s | 12,47 s / 3,03 s |
| mips64el : les mêmes | 11,87 s / 1,76 s | 11,65 s / 1,70 s |
| x86_64 : Python (json, re, math) | 1,81 s | 1,47 s |
| x86_64 : `xz -2` | 2,85 s | 2,55 s |
| x86_64 : `sort -n` | 1,30 s | 1,06 s |
| x86_64 : `sha256sum` | 0,22 s | 0,17 s |

  Les programmes x86_64 sont ceux de l'hôte, lancés par `qemu-x86_64`. Cette comparaison a révélé un ralentissement de 13 % de `xz` sous `qemu-x86_64`, causé par le test des clés génériques dans `tb_lookup()` ; il est corrigé (commit « accel/tcg: generic TB flags out of the tb_lookup() hit path »).
- **Un défaut de QEMU 11.1.1, non corrigé ici.** Dans un QEMU compilé avec les plugins (le défaut de `configure`), un processus multithread qui fait `fork()`, puis crée un thread dans l'enfant, fait échouer une assertion (`plugins/core.c`, `qemu_plugin_vcpu_init__async`). QEMU 11.1.1 d'origine fait de même. Les binaires de `rejit/bin`, compilés sans plugins, ne sont pas concernés.

### Écarts connus avec le QEMU d'origine

- **ARM** : aucun connu. Les tests comparent aussi FPSR et FPSCR bit à bit. Les cas que l'hôte ne sait pas reproduire passent par les helpers d'origine :
  - FPCR non standard ;
  - `FRINTA`/`FCVTA*` (arrondi « ties away ») ;
  - `FMULX` ;
  - demi-précision ;
  - vecteurs 2S ;
  - flottant NEON d'A32, qui utilise le « Standard FPSCR » avec flush-to-zero.
- **PowerPC** : seulement des détails du FPSCR. VXSOFT est utilisé au lieu du sous-type d'invalide, et FI est cumulé. Voir [README-ppc.md](README-ppc.md).
- **RISC-V**
  - Une faute dans un `vle`/`vse` traité en ligne laisse `vstart` à 0 : l'instruction repartira du début. Le helper, lui, met `vstart` à l'indice de l'élément fautif. Les éléments d'avant la faute peuvent déjà être chargés ou rangés ; en mémoire ordinaire, relancer l'instruction les refait à l'identique.
  - Pour une telle faute, `si_addr` est l'adresse de l'accès de l'hôte. Si un élément chevauche deux pages, elle peut différer de moins d'un élément de celle du helper.
  - Les cas non couverts passent par les helpers d'origine, exacts :
    - modes d'arrondi autres que RNE ;
    - VLEN de 512 bits ou plus pour les accès vectoriels ;
    - segments, accès stridés ou indexés ;
    - flottant vectoriel masqué, avec `vl < VLMAX` ou avec un LMUL fractionnaire ;
    - politiques « agnostic » qui écrivent des 1 (`rvv_ta_all_1s`, `rvv_ma_all_1s`).

## Réponses aux contraintes demandées

- **Même dépôt, même base de code.** Un seul arbre QEMU et un seul `configure` pour toutes les cibles. Les opérations TCG, le backend AVX-512, le jump cache et les accélérations de la traduction sont communs. Les front-ends `target/ppc`, `target/arm` et `target/riscv` ne font que les utiliser.
- **Compilateurs croisés.** Ce sont ceux des paquets Ubuntu, natifs x86 et non émulés : `gcc-aarch64-linux-gnu`, `gcc-arm-linux-gnueabihf`, `gcc-riscv64-linux-gnu` et `gcc-powerpc64le-linux-gnu` (GCC 13.3), ou un GCC croisé compilé pour Power11.
- **Appels C remplacés par du code JIT.** Le JIT de QEMU (TCG) est réutilisé tel quel. Ce qui était un appel de helper devient une opération TCG, émise en instructions de l'hôte. Les helpers ne restent que comme chemin lent pour les cas rares.
- **AVX-512 et ses 32 registres.**
  - Sans AVX-512 (BW, DQ, VL ; VBMI pour les permutations ; SHA pour SHA-256), rien ne casse : les instructions repassent par les helpers.
  - L'allocateur de TCG ne gère que `xmm0` à `xmm15`. Le code FP utilise `xmm16` à `xmm23` et les registres de masque comme temporaires.
  - Les registres de masque servent aussi aux vecteurs RISC-V : éléments sous `vl`, bits de `v0`, résultats des comparaisons.
- **Load/store.**
  - Les registres invités sont chargés depuis `env`, gardés en registres hôte dans un bloc, puis réécrits.
  - Les opérations elles-mêmes travaillent de registre à registre.
  - La vivacité de TCG libère les registres morts. Un accès mémoire sans chemin lent ne les vide plus inutilement.
- **Dolphin.** Comme dans son JIT PowerPC, le flottant invité est exécuté par le flottant de l'hôte :
  - des correctifs couvrent les points où les sémantiques divergent (NaN, simple précision, drapeaux) ;
  - un chemin lent sert quand l'état sort du cas courant (arrondi dirigé, flush-to-zero, exceptions activées).
- **x87.** Il n'est utilisé nulle part.
  - RISC-V, ARM et Power demandent du binary32/binary64 IEEE exact. Or le x87 calcule en 80 bits ; même réglé sur 53 bits, son exposant reste étendu, d'où un double arrondi sur les sous-normaux. Pour être exact, il faudrait ranger chaque résultat en mémoire et le relire (ce que fait GCC avec `-ffloat-store`).
  - Il n'a pas de transfert direct avec les registres XMM : tout passe par la mémoire.
  - Ses atouts, le format 80 bits et `fsin`/`fyl2x`, ne servent à aucune de ces cibles. Aucune n'a d'instruction qui s'y ramène : leur libm calcule `sin` ou `log` avec des opérations ordinaires. Et leur `long double` est soit un `double` (armhf), soit un format de 128 bits.

## Pistes

- **Coût de traduction.** Pour les programmes courts (`grep`, `dpkg`, `dnf`, `sort`), la traduction pèse encore 20 à 50 % du temps. Un cache de traduction persistant entre processus serait le gain suivant.
- **Émulation système.** Le flottant en code généré y demanderait de reporter les drapeaux de MXCSR dans l'état du CPU à chaque sortie de `cpu_exec()` (plusieurs vCPU par thread en TCG « round-robin », lecture des registres par la migration, le gdbstub, le moniteur).
- **Défauts du cache de PAC.** Ils restent de 3 à 4 % sur Python et gzip. Un cache plus grand les supprime, mais ne gagne rien en temps : la pression sur le cache L1 compense.
- **Instructions encore en helpers.**
  - ARM :
    - `FRINTA`/`FCVTA*` et les conversions vectorielles vers entier avec arrondi dirigé ;
    - `FRECPE`/`FRSQRTE`, la demi-précision, AES/SHA-1/SHA-512 ;
    - les FP 2S ;
    - le flottant NEON d'A32, qui demanderait de basculer FTZ/DAZ dans MXCSR ;
    - `RBIT`.
  - RISC-V :
    - les accès vectoriels à segments, stridés et indexés ;
    - les réductions (`vredsum`, `vfredusum`…), `vslide*`, `vrgather`, `vcompress` ;
    - les opérations élargissantes et rétrécissantes ;
    - le flottant vectoriel masqué ou avec `vl < VLMAX` ;
    - Zvkned/Zvknh (AES, SHA-2), qui iraient vers AES-NI et SHA-NI.
- **Code entier.** Le reste du temps de gzip et xz est du code entier que TCG traduit déjà directement. La qualité de ce code (allocation de registres sur plusieurs blocs) est un chantier à part.
