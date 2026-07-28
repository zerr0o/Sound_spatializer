# Développement Windows

## Outils

- Windows 11 24H2 x64 ;
- Visual Studio 2022, charge C++ Desktop ;
- Windows Driver Kit 10.0.26100 ;
- CMake 3.24 ou plus récent ;
- Rust stable et Tauri CLI 2 ;
- Node.js 22 et pnpm 10 ;
- Python 3.11+ pour les tests d'outillage.

## Interface et moteur

```powershell
pnpm install
pnpm --dir apps/desktop assets:sync
pnpm check
pnpm test:ui
pnpm build:ui

.\tools\build-engine.ps1 -Configuration Release
```

Pour travailler dans la fenêtre native Tauri avec le serveur Vite :

```powershell
pnpm build:engine:debug
pnpm --dir apps/desktop tauri dev
```

Le premier appel construit le moteur Debug avec libmysofa. L'hôte Tauri Debug
préfère `build/engine-mysofa/Debug/SoundSpatializer.Engine.exe` à une copie
ancienne dans `target/debug`; relancez la commande après toute modification
native. `pnpm build:engine:dev` fournit sinon une variante analytique plus légère
sans import SOFA.

`pnpm build:frontend` ne construit que les fichiers web. `pnpm build:ui` passe
obligatoirement par `tauri build --no-bundle --features custom-protocol`, fusionne
`tauri.production.conf.json` pour retirer `devUrl`, embarque `frontendDist`, puis
inspecte le binaire. Un `cargo build --release` direct est volontairement refusé :
sans la feature `custom-protocol`, Tauri ouvrirait encore
`http://127.0.0.1:1420` après livraison.

Le build standard restaure la version épinglée de libmysofa via `vcpkg.json` ;
le bootstrap désactive explicitement les métriques vcpkg. Un backend analytique
est disponible uniquement pour les tests de développement avec
`.\tools\build-engine.ps1 -Configuration Debug -WithoutMySofa`.

```powershell
$toolchain = .\tools\bootstrap-vcpkg.ps1
$repository = (Resolve-Path .).Path
cmake -S native/engine -B build/engine-mysofa `
  -DCMAKE_TOOLCHAIN_FILE=$toolchain `
  -DVCPKG_MANIFEST_DIR=$repository `
  -DVCPKG_INSTALLED_DIR="$repository\vcpkg_installed" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DSOUND_SPATIALIZER_ENABLE_MYSOFA=ON `
  -DBUILD_TESTING=ON
cmake --build build/engine-mysofa --config Release --parallel
ctest --test-dir build/engine-mysofa -C Release --output-on-failure
```

Une construction livrable doit charger libmysofa et passer les tests d'import
avec les fichiers SOFA vérifiés ; le backend analytique n'est qu'un secours de
développement.

Les trois exécutables livrés sont construits avec le CRT MSVC statique : le MSI
ne dépend donc pas de la présence préalable du redistribuable Visual C++ sur une
installation Windows propre. Toute modification des triplets vcpkg, des
`rustflags` ou du runtime CMake doit être suivie d'un contrôle `dumpbin
/dependents`.

## Ressources locales

Le modèle MediaPipe est téléchargé depuis le dépôt officiel Google, puis contrôlé
par taille et SHA-256 avant renommage atomique. Les archives SADIE II sont
contrôlées par taille et MD5 conformément aux métadonnées Zenodo ; seuls les
membres SOFA attendus sont extraits.

Le Worker importe statiquement le chargeur ES6 officiel de `tasks-vision`.
Seuls `vision_wasm_module_internal.wasm` et le modèle `.task` sont servis depuis
`public/` : importer dynamiquement le chargeur JavaScript depuis ce répertoire
est interdit par Vite et casse également la portée globale de `ModuleFactory`
dans un Worker module. Le build exécute `verify-mediapipe-build.mjs` pour bloquer
toute régression de ce chemin hors ligne.

```powershell
.\tools\fetch-hrtf.ps1 -ProfileId sadie-d2-kemar
pnpm --dir apps/desktop assets:sync
```

Les fichiers `.sofa` restent ignorés par Git. Leur notice et leur manifeste sont
versionnés.

La sélection des cinq sujets humains est reproductible, mais sa régénération
nécessite les archives complètes `H3.zip` à `H20.zip` du record SADIE II v2-2.
Après les avoir placées dans un dossier de travail ignoré :

```powershell
python .\tools\extract-sadie-anthropometry.py .\build\research `
  .\resources\hrtf\anthropometry-v2-2.csv
python .\tools\select-hrtf-profiles.py `
  .\resources\hrtf\anthropometry-v2-2.csv --count 5 --seed 42
```

Le CSV ne contient que largeur, hauteur et profondeur en millimètres dérivées
des OBJ publics ; les scans et images biométriques ne sont ni versionnés ni
distribués dans l'application.

## Pilote

Les scripts sous `native/driver/scripts` restaurent un commit SysVAD épinglé,
appliquent la surcouche Sound Spatializer puis construisent le package WDK. La
préparation et la compilation ne modifient pas les périphériques de la machine.

Le MSI n'est pas nécessaire pour développer. Construire d'abord le package
Debug et son certificat public depuis un PowerShell normal :

```powershell
pnpm driver:check
pnpm driver:build:test-signed
```

La seconde commande génère
`native/driver/artifacts/driver/x64/Debug/development-package.json`. Elle crée
le certificat de signature dans `CurrentUser\My`, mais ne l'approuve pas au
niveau machine et n'installe aucun périphérique.

Sur une machine de test déjà préparée par son administrateur, ouvrir ensuite un
PowerShell élevé dans la racine du dépôt :

```powershell
pnpm driver:install
# développer et tester l'application
pnpm driver:uninstall
```

`driver:install` relit le manifeste, vérifie les chemins absolus, l'INF, le SYS,
le catalogue, l'empreinte et le certificat public avant toute mutation. Il
refuse ensuite l'installation si Secure Boot n'est pas explicitement désactivé
ou si Windows TESTSIGNING n'est pas vérifié actif, puis délègue une installation
transactionnelle. `driver:uninstall` retire le périphérique, son package publié
et le certificat public ajouté à `LocalMachine`; la clé privée de développement
dans `CurrentUser\My` est conservée.

L'installation d'un pilote test-signé change l'état système et doit être faite
sur une machine ou VM dédiée avec un certificat de développement approprié. Ne
pas désactiver Secure Boot ou les contrôles de signature sur un poste de travail
pour contourner une erreur de package. Aucun script du dépôt ne modifie Secure
Boot, l'UEFI, BitLocker ou le BCD. Avec Secure Boot actif, il faut un package
signé par Microsoft pour charger le pilote kernel.

Avant toute distribution : analyse statique, Driver Verifier, HLK, cycle complet
install/mise à jour/rollback/désinstallation, veille/reprise, signature Microsoft
et validation sur une installation propre.

## Endpoint render tiers

Un endpoint virtuel render déjà présent sur la machine peut servir de source de
compatibilité sans installer le pilote Sound Spatializer. Le dépôt ne choisit
aucun produit tiers, ne le télécharge pas, ne l'installe pas et ne le redistribue
pas. L'utilisateur doit sélectionner explicitement l'identifiant WASAPI de la
source et un casque physique distinct.

Dans une scène V1, la route native canonique est :

```json
"captureProvider": "native-driver",
"captureEndpointId": null
```

`pnpm build:ui` place le moteur Release à côté du payload Tauri dans
`apps/desktop/src-tauri/target/release`. Un usage sans installateur doit copier
les deux exécutables ensemble ; le test d'artefact échoue si le moteur compagnon
est absent ou n'est pas un fichier PE Windows.

La route tierce utilise `captureProvider: "external-render"` et un
`captureEndpointId` non vide. La commande `set-audio-route` change cette source
et `outputDeviceId` en une seule opération; elle refuse l'endpoint absent,
inactif ou identique à la sortie sans conserver une demi-configuration. Omettre
les deux nouvelles propriétés conserve le comportement historique natif.

Ce chemin reste un mode de compatibilité non qualifié. Il est indépendant de
`audio.mode: "compatibility"`, qui signifie uniquement une cible de tampon de
256 frames. Une mesure physique est indispensable avant de lui attribuer les
objectifs de latence ou de stabilité du mode professionnel.

## Discipline temps réel

Les tests unitaires ne suffisent pas à prouver l'absence d'allocation. Pour une
qualification, instrumenter les callbacks avec ETW/WPA et un allocateur sentinelle,
faire dériver les horloges ±1000 ppm et exécuter le soak de 24 h. Toute modification
DSP doit conserver les golden impulses des quatre chemins source→oreille.
