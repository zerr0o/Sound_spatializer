# Pilote audio virtuel Sound Spatializer

Ce dossier contient un dérivé minimal de SysVAD/WaveRT pour Windows 11 24H2
x64. Il expose un seul endpoint de rendu `Sound Spatializer`, au format partagé
fixe 48 kHz, stéréo, IEEE float32. Il n'expose aucun endpoint de capture.
L'entrée du moteur utilisateur est le loopback WASAPI de cet endpoint de rendu.

## Etat réel

Le code est un socle de développement, pas un pilote distribuable. Le package
Debug peut être compilé et auto-signé localement avec le WDK, mais il n'a pas
encore été exécuté sous Driver Verifier, qualifié HLK, validé en chargement avec
Secure Boot, ni signé par Microsoft. Ces étapes sont des critères de sortie
obligatoires; aucun script ne les contourne.

La dépendance Microsoft `Windows-driver-samples` est épinglée au commit
`1fd430c78971c31b624b0773bbea825d8b480d55`. Elle est restaurée sous
`native/driver/build/_deps`, puis le patch versionné de `patches/` est appliqué.
Le code Microsoft reste sous licence MIT; voir `THIRD_PARTY_NOTICES.md`.

## Contrat du périphérique

- Hardware ID: `Root\SoundSpatializer_Audio`
- Interface stable: `{EF58434D-ADA7-47E2-A2C4-4E8C58BA3E0B}`
- Property set fournisseur: `{B01E7F02-85B0-4CF9-B53D-75DFD2B05E07}`
- Références KS: `WaveSoundSpatializer` et `TopologySoundSpatializer`
- Format unique: 48 000 Hz, 2 canaux, 32-bit IEEE float
- Endpoint: render-only, loopback activé, WaveRT event-driven

Le moteur doit découvrir l'endpoint avec l'interface et les propriétés stables,
jamais avec le nom affiché. Le pilote ne change jamais la sortie Windows par
défaut et n'expose aucun IOCTL de contrôle produit.

## Transport render vers loopback

Le loopback de démonstration SysVAD produit normalement une sinusoïde. Ce
comportement est désactivé. Le miniport possède un historique cyclique non paginé
préalloué de 64 KiB qui reçoit exactement les octets consommés par le DMA WaveRT
virtuel. Le pin loopback relit ces octets; un underrun produit du silence et un
overrun saute vers le suffixe le plus récent au lieu de rejouer un ancien son.
Les chemins `SaveData` (allocation, fichier, work item et écriture) sont retirés.

La commande de protection loopback fournie par le moteur audio Windows mute ce
transport réel, et non plus seulement la tonalité d'exemple SysVAD. Les
transitions entrent et sortent au bord temps réel afin qu'aucun bloc protégé
accumulé pendant une pause ne soit rejoué après démute.

Les deux copies kernel sont sérialisées par un `KSPIN_LOCK` de très courte durée
afin qu'une réécriture circulaire ne puisse pas déchirer une lecture concurrente.
Cette protection, bornée et sans allocation/I/O, appartient au pont WaveRT dans
le pilote. Elle ne change pas l'invariant séparé du callback MMCSS du moteur
utilisateur, qui doit rester sans verrou, allocation, journalisation ni attente.

Le harness C++ vérifie le transfert bit à bit, le silence, la récupération
d'overrun, la protection loopback et un stress producteur/consommateur:

```powershell
.\native\driver\scripts\Test-DriverTransport.ps1 -Configuration Release
```

## Compiler

Prérequis sur une machine de développement dédiée:

- Visual Studio 2022 avec C++ x64;
- Windows 11 SDK et WDK compatibles 10.0.26100;
- intégration WDK/VSIX active;
- CMake 3.24+ pour le harness indépendant.

```powershell
.\native\driver\scripts\Prepare-SysVad.ps1
.\native\driver\scripts\Build-DriverTools.ps1 -Configuration Release
.\native\driver\scripts\Build-Driver.ps1 -Configuration Debug
```

`SoundSpatializer.DriverCtl.exe` est un helper SetupAPI/NewDev compilable avec le
SDK Windows seul. Il crée le devnode root sans dépendre de DevCon sur une machine
cliente et le retire si la liaison initiale échoue. `Build-Driver.ps1` échoue
explicitement si le WDK manque. La sortie attendue est
`native/driver/artifacts/driver/x64/<Configuration>`. `InfVerif` est exécuté
quand l'outil est présent. Les projets et le script épinglent le kit
`10.0.26100.0`; une compilation réussie ne vaut ni HLK ni signature.

## Signature et installation de développement

Le parcours court, sans MSI, est disponible à la racine du dépôt :

```powershell
pnpm driver:check
pnpm driver:build:test-signed
# Dans un nouveau PowerShell élevé, sur une machine de test déjà préparée :
pnpm driver:install
pnpm driver:uninstall
```

`driver:install` consomme uniquement le
`artifacts/driver/x64/Debug/development-package.json` fraîchement généré. Il
valide le package, le helper, l'empreinte et le certificat public avant de
demander une installation transactionnelle. Il refuse toute politique de
chargement incompatible avant d'ajouter le certificat aux magasins machine ou
de toucher à PnP. Le dépôt ne modifie jamais Secure Boot, UEFI, BitLocker ou le
BCD.

La création du certificat n'exporte jamais la clé privée et ne modifie ni
Secure Boot, ni BCD, ni le mode `testsigning`:

```powershell
$cert = .\native\driver\scripts\New-TestCertificate.ps1
.\native\driver\scripts\Sign-TestPackage.ps1 `
  -PackageDirectory .\native\driver\artifacts\driver\x64\Debug `
  -Thumbprint $cert.Thumbprint
```

Sur une machine de test isolée, l'administrateur peut ensuite approuver
explicitement le certificat public et installer le package:

```powershell
.\native\driver\scripts\Install-Driver.ps1 `
  -PackageDirectory .\native\driver\artifacts\driver\x64\Debug `
  -TrustTestCertificate `
  -CertificatePath $cert.PublicCertificate
```

Le script refuse un catalogue absent, non signé, non approuvé ou signé par un
autre certificat. Il utilise le helper SetupAPI pour créer ou mettre à jour
l'instance root; PnPUtil n'est qu'un repli de développement si une instance
existe déjà. Pendant une mise à jour MSI, l'ancien `oem*.inf` reste disponible
jusqu'au commit afin de permettre un rollback, puis les packages devenus
obsolètes sont retirés du Driver Store. Il ne sélectionne jamais l'endpoint
comme sortie par défaut.

```powershell
.\native\driver\scripts\Uninstall-Driver.ps1 `
  -RemoveTestCertificate `
  -CertificateThumbprint $cert.Thumbprint
```

La désinstallation retire les instances PnP exactes, le `oem*.inf` identifié et,
sur demande seulement, le certificat public ajouté à `Root` et
`TrustedPublisher`. La clé privée de développement reste dans `CurrentUser\My`.

## Validation encore bloquante

- build Debug/Release WDK et analyse statique sans warning;
- Driver Verifier, sommeil/reprise, hotplug et changements d'alimentation;
- InfVerif, HLK complet et installation/mise à jour/rollback/désinstallation;
- vérification bit à bit avec un client WASAPI réel;
- test 24 h sans dropout et mesure physique de latence;
- signature d'attestation ou HLK Microsoft pour la distribution publique.
