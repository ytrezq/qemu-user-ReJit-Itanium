# PowerPC (ppc64le, Power11) : détail des changements

Ce document complète la section « PowerPC » de [README.md](README.md). Il décrit le travail sur `qemu-ppc64le` : ce qui passe en code AVX-512, la gestion paresseuse du FPSCR et les écarts connus. Les mesures à jour des quatre cibles sont dans le README principal.

QEMU (TCG) est déjà un JIT : il traduit le code PowerPC en code x86 par blocs. En revanche, le flottant, le VSX, le MMA et plusieurs instructions vectorielles y passaient par des **fonctions C** : softfloat, un appel par instruction, soit 30 à 130 fois plus lent que le code natif. Ces appels sont remplacés par du **code généré** qui utilise directement les instructions AVX-512 de l'hôte. La sémantique PowerPC est conservée : règles des NaN, FPSCR, arrondi simple précision, saturation des conversions.

Tout cela ne concerne que l'émulation utilisateur (`qemu-ppc64le`, `qemu-ppc64`, `qemu-ppc`). L'émulation système (`qemu-system-ppc*`) garde les helpers C d'origine pour le flottant.

## Ce qui a été modifié

### 1. Branchements indirects

- **Jump cache sondé en ligne** : le cache de sauts (`blr`, `bctr`) est sondé dans le code généré, au lieu d'appeler `helper_lookup_tb_ptr` (environ 30 % du temps de certaines charges).
- **Cache agrandi** : 16 K entrées au lieu de 4 K, avec un hachage qui ignore les bits nuls des adresses d'instructions.

### 2. Flottant : nouvelles opérations TCG

Les nouvelles opérations sont `fpop1/2/3_vec`, `fpcmpcc_vec` et `perm2b_vec` (`include/tcg/tcg-fpop.h`). Le backend x86-64 les émet en EVEX (`tcg/x86_64/tcg-target-fpop.c.inc`) :

- **Registres et constantes**
  - Les registres opmask `k1` à `k3` servent aux voies scalaires, aux correctifs NaN sans branchement et aux accès masqués.
  - `xmm16` à `xmm23` servent de registres temporaires : l'allocateur TCG ne connaît que `xmm0` à `xmm15`.
  - Les constantes viennent d'un pool, par diffusion `{1toN}`.
- **NaN à la manière PowerPC**
  - Le NaN généré est positif (celui de x86 est négatif).
  - Priorité A, C, B pour les FMA.
  - `0×∞ + qNaN` est signalé invalide (VXIMZ), ce que x86 ne fait pas.
- **Simple précision** (`fadds`, `xsmulsp`, `fmadds`…) : calcul en double avec **arrondi vers l'impair**, émulé par `{ru-sae}` et `{rd-sae}`, puis un seul arrondi en simple. Le résultat est exact même si les opérandes ne sont pas des flottants 32 bits.
- **Underflow**
  - PowerPC détecte la « petitesse » avant arrondi, x86 après.
  - Un résultat arrondi au plus petit normal est donc revérifié hors du chemin rapide, et UX est levé si besoin.
- **Conversions** vers entier : saturation et valeurs de NaN PowerPC (`fctiw`, `xscvdpsxws`…).
- **maxNum/minNum** (`xsmaxdp`, `xvminsp`…) : `vrange`, puis correctifs NaN.

### 3. FPSCR paresseux

- **Accumulation dans MXCSR** : les exceptions levées par le code AVX-512 s'y accumulent.
- **FPRF paresseux** : il est gardé sous la forme du résultat qui le définit.
- **Reconstruction** : `ppc_fpscr_sync()` reconstruit FPSCR seulement quand il est lu, par exemple :
  - `mffs`, `mcrfs`, `Rc=1` ;
  - helpers ;
  - syscalls, signaux, gdb.
- **Arrondi et exceptions activées**
  - MXCSR reste en arrondi au plus proche.
  - Un mode d'arrondi dirigé ou une exception FP activée (`feenableexcept`) fait repasser les instructions par les helpers C, via le bit `HFLAGS_FP_SOFT` de la clé des blocs traduits.
  - Les blocs se terminent dès qu'une instruction peut changer cet état.
- **Flags parasites de l'hôte**
  - Des flags peuvent être levés par du code hôte : glib pendant la traduction, les syscalls, le hardfloat d'Altivec.
  - Ces chemins sauvegardent et restaurent MXCSR.
  - Le gestionnaire de signaux hôte recharge le MXCSR interrompu avant un `siglongjmp`.

### 4. Instructions compilées en AVX-512 au lieu d'appels C

- **FP classique**
  - Arithmétique : `fadd(s)` `fsub(s)` `fmul(s)` `fdiv(s)` `fsqrt(s)`.
  - FMA : `fmadd` `fmsub` `fnmadd` `fnmsub` (et formes s).
  - Arrondis : `frsp` `friz` `frip` `frim`.
  - Conversions : `fcti[w|d][u][z]` `fcfid[s|u|us]`.
  - Autres : `fcmpu` `fcmpo` `fsel`, conversions de `lfs` et `stfs`.
- **VSX scalaire et vectoriel**
  - Arithmétique : `xs/xv add/sub/mul/div` (sp et dp), FMA formes A et M, `sqrt`.
  - Comparaisons : `xscmpu/odp`, `xscmpeq/gt/gedp`, `xvcmp*` avec formes record.
  - Max/min : `xsmax/min[c]dp`, `xvmax/min`.
  - Conversions entier ↔ flottant, arrondis `xsrdpi[cmpz]` et `xvr[ds]pi[cmpz]`, `xsrsp`.
- **MMA (Power10/11)** : `xvf32ger*` et `xvf64ger*`, y compris les formes préfixées sans masque. Chaque instruction devient 4 FMA vectorielles au lieu de 16 opérations softfloat.
- **Altivec/VSX**
  - `vshasigmaw/d` : SHA-2 avec rotations AVX-512.
  - `vsldoi`.
  - `vperm`, `vpermr`, `xxperm`, `xxpermr` : un seul `vpermi2b`.
- **`lxvl`, `lxvll`, `stxvl`, `stxvll`**
  - Ce sont les chaînes glibc Power10. Le helper faisait un accès MMU par octet.
  - Ici : un seul `vmovdqu8` masqué, qui ne fait pas de faute sur les octets exclus. Nouvelles opérations TCG `ldm_vec` et `stm_vec`, en mode utilisateur.

### 5. Divers

- **Processeur par défaut** : `power11` au lieu de POWER9. glibc choisit alors ses variantes `glibc-hwcaps/power10`.
- **Branchements** : ils n'enregistrent plus de BHRB en mode utilisateur. MMCRA[BHRBRD] est mis comme le fait Linux, et le CFAR, illisible en mode problème, n'est pas suivi.
- **Store forwarding** : un vecteur de 16 octets est chargé depuis `env` en deux fois 8 octets. PowerPC écrit souvent les moitiés d'un VSR séparément, et un seul chargement de 16 octets bloquait le store forwarding, ce qui valait ×3 sur les boucles vectorielles.

## Validation

`tests/ppc/run-tests.sh <qemu-référence>` compare le JIT au QEMU d'origine. Environ 1,5 million de cas par exécution, plus de 4 millions au total pendant le développement :

- valeurs spéciales (±0, dénormaux, ±∞, qNaN et sNaN avec charge utile) et bornes des conversions ;
- valeurs aléatoires ;
- toutes les instructions listées ci-dessus, avec le FPSCR lu par `mffs` ;
- séquences d'instructions entre deux lectures du FPSCR ;
- changement de mode d'arrondi au milieu d'un bloc ;
- `feenableexcept` et SIGFPE ;
- signaux et fautes au milieu du code FP ;
- threads et `fork` ;
- `lxvl`/`stxvl` au bord d'une page non mappée.

Autres vérifications :

- **Python** : les tests `test_math`, `test_float`, `test_cmath`, `test_complex`, `test_fractions`, `test_statistics`, `test_strtod`, `test_decimal`, `test_fstring`, `test_struct`… passent dans le chroot AlmaLinux 10.2. Les échecs sont identiques avec le QEMU d'origine.
- **Build `--enable-debug-tcg`** : toutes les assertions internes de TCG passent sur les tests et les charges.

## Écarts connus avec le QEMU d'origine

Aucun écart sur les résultats numériques pour des opérandes définis par l'ISA. Les écarts sur le FPSCR sont les suivants :

1. **Détail des exceptions invalides.** L'hôte n'a qu'un indicateur « invalide ». Il est reporté dans VXSOFT et VX, au lieu de VXSNAN, VXISI, VXIDI… `FPSCR[VX]`, `FX`, `fetestexcept(FE_INVALID)` et SIGFPE restent exacts.
2. **FI** indique qu'une des instructions depuis la dernière lecture du FPSCR était inexacte, et pas forcément la dernière.
3. Cas où **le JIT suit l'ISA et le QEMU d'origine ne la suit pas** :
   - QEMU arrondit deux fois `xsaddsp`, `xsmulsp`, `xsmaddasp`, `xssqrtsp`… quand les opérandes ne sont pas des flottants 32 bits.
   - QEMU perd UX quand un autre élément du même vecteur déborde.
   - QEMU met FPRF[C] après un `fcmpo` sur un NaN, alors que l'ISA le laisse inchangé.
   - Après une conversion `fcti*` invalide, FPRF est indéfini selon l'ISA.

## Pistes

- **CR0 paresseux** pour les instructions `Rc=1` suivies d'un branchement.
- **Helpers restants.** `vbpermq` (`vpshufbitqmb`), `vctzlsbb` et `cmpb` pèsent encore jusqu'à 1,5 %.
