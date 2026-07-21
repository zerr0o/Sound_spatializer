# Installateur WiX

Ce projet WiX 5.0.2 x64 assemble l'interface Tauri, le moteur utilisateur et le
package pilote. Il exécute l'installation du pilote en élévation, programme une
action de rollback et retire le pilote avant les fichiers lors d'une
désinstallation. Une mise à niveau majeure conserve le nouveau package pilote
au lieu de le supprimer avec l'ancien MSI. Il installe aussi les six SOFA SADIE
II attendus par le host dans l'unique arborescence canonique
`resource_dir()/hrtf`: KEMAR/D2, H6, H9, H10, H19 et H20. Le modèle Face
Landmarker et les variantes SIMD/non-SIMD du runtime WASM restent des assets
Vite embarqués une seule fois dans l'exécutable Tauri; le MSI ne les duplique
pas comme fichiers externes. L'index global des dépendances et les notices
spécifiques au pilote et à l'outil de build WiX sont installés à côté des
exécutables. Avant une diffusion, cet index doit être complété par les textes
intégraux produits à partir des lockfiles npm, Cargo et vcpkg.

Le composant moteur crée une valeur machine
`HKLM\Software\Microsoft\Windows\CurrentVersion\Run\SoundSpatializer.Engine`
qui lance directement `SoundSpatializer.Engine.exe --autostart`, sans shell ni
PowerShell. Le moteur Release est une application Win32 sans console et protège
chaque utilisateur par mutex et pipe nommés avec son SID. La valeur appartient
au même composant MSI que l'exécutable: elle est donc annulée lors d'un rollback,
mise à jour avec le chemin du produit et supprimée à la désinstallation.

Ce MSI est l'unique installateur produit autorisé, car lui seul orchestre le
devnode et le cycle de vie du pilote. Le binaire Tauri doit être produit sans
bundle concurrent avec `pnpm build:ui`, qui exécute
`tauri build --no-bundle --features custom-protocol`, active le protocole de
production, retire `devUrl` via la configuration de livraison, embarque
`frontendDist` et refuse toute URL
de serveur Vite résiduelle. Le binaire ainsi vérifié est passé via
`DesktopExecutable`. Un NSIS/MSI généré directement par Tauri ne
contiendrait pas les actions transactionnelles du pilote et ne doit être ni
publié ni présenté comme un package installable.

`SoundSpatializer.DriverCtl.exe` crée le devnode root avec SetupAPI puis lie ou
met à jour le package avec NewDev. L'installateur ne dépend donc pas de DevCon ou
du WDK sur la machine cliente. Si la liaison échoue après création, le helper
retire immédiatement le devnode créé. En mode transactionnel, le script capture
le package `oem*.inf` actif et l'état d'installation avant toute mutation. Une
action rollback force la restauration de cet INF avec NewDev (ou retire le
nouveau devnode lors d'une première installation), puis une action commit efface
le snapshot seulement après succès de la transaction MSI.

Le MSI de production refuse naturellement un pilote dont le catalogue n'est pas
déjà approuvé par Windows. Il n'embarque, n'importe et ne crée aucun certificat
de test. Les scripts de test restent un workflow séparé, réservé à une machine
de développement. L'option PowerShell `ExecutionPolicy Bypass` des actions MSI
ne concerne que l'exécution non interactive des scripts déjà extraits sous
`Program Files`; elle ne désactive ni l'intégrité du noyau, ni Secure Boot, ni
la validation PnP du catalogue. Aucun appel à BCD ou au mode `testsigning`
n'existe dans l'installateur.

WiX 5.0.2 est volontairement épinglé: WiX 6+ relève de l'Open Source
Maintenance Fee et WiX 7 exige en plus une acceptation explicite de son EULA.
Le toolset WiX 5 est un outil de build MS-RL récupéré par NuGet; ses binaires ne
sont pas redistribués avec le produit. Toute montée de version demande un audit
juridique et une décision explicite, jamais une acceptation automatique en CI.
Le SDK est référencé avec une version exacte et une cible MSBuild bloque aussi
toute version effective différente de 5.0.2.

Deux variantes intentionnellement incompatibles sont disponibles:

- `PackageMode=Production` (valeur par défaut) produit
  `bin/x64/Production/SoundSpatializer.Production.Installer.msi`. Il exige le
  véritable INF/SYS/CAT, un catalogue dont la chaîne est approuvée sur la
  machine de build et DriverCtl. Lui seul contient les quatre custom actions
  transactionnelles et l'autostart du moteur.
- `PackageMode=Preview` produit
  `bin/x64/Preview/SoundSpatializer.Preview.Installer.msi`. Ce package est une
  prévisualisation applicative sans pilote: aucun INF/SYS/CAT, script pilote,
  DriverCtl, custom action PnP ou autostart n'y est inclus. Son ProductName,
  UpgradeCode, dossier `Program Files` et tous ses GUID de composants sont
  distincts, afin qu'il ne répare, ne mette à niveau et ne partage aucun
  composant avec Production.

Le mode applicatif Preview peut utiliser un endpoint render tiers uniquement si
celui-ci a déjà été installé séparément et sélectionné explicitement par
l'utilisateur. Le MSI ne télécharge, n'installe, ne répare et ne redistribue
aucun pilote audio tiers. Cette route reste une compatibilité non qualifiée et
ne transforme pas Preview en package Production. Elle est indépendante de
`audio.mode=compatibility`, qui ne règle que la cible de tampon à 256 frames.
La validation atomique source/sortie et l'interdiction de boucle appartiennent
au moteur, jamais à une custom action WiX.

L'ancien interrupteur `AllowPlaceholderPayloads` a été supprimé et sa présence
fait désormais échouer le build. Il permettait de créer un MSI apparemment
installable qui lançait ensuite `InstallDriver` avec trois fichiers factices;
Windows Installer signalait alors 1722 puis 1603. Le build supprime aussi les
anciens artefacts ambigus sous `bin/x64/Release`.

Dans les deux modes, `DesktopResourcesDirectory` doit pointer sur le staging
Tauri contenant `hrtf/`, synchronisé et vérifié par le build UI.
`Test-HrtfPayload.ps1` revalide indépendamment le schéma du manifeste,
l'ensemble exact D2/H6/H9/H10/H19/H20, l'absence de SOFA parasite, les tailles
et chaque SHA-256. La compilation échoue si un fichier applicatif déclaré
manque ou est vide. Production exécute en plus `Verify-DriverPackage.ps1` avant
la création du MSI et refuse explicitement l'ancien marqueur de smoke.

Pour construire puis inspecter la variante Preview, sans mutation PnP:

```powershell
dotnet build .\installer\wix\SoundSpatializer.Installer.wixproj -c Release `
  -p:PackageMode=Preview -p:ProductVersion=0.1.0

.\installer\wix\Test-PackageFlavor.ps1 `
  -MsiPath .\installer\wix\bin\x64\Preview\SoundSpatializer.Preview.Installer.msi `
  -ExpectedMode Preview
```

Build Production avec des payloads Release déjà produits:

```powershell
dotnet build .\installer\wix\SoundSpatializer.Installer.wixproj -c Release `
  -p:PackageMode=Production `
  -p:ProductVersion=0.1.0 `
  -p:DesktopExecutable=C:\payload\SoundSpatializer.exe `
  -p:EngineExecutable=C:\payload\SoundSpatializer.Engine.exe `
  -p:DesktopResourcesDirectory=C:\payload\resources `
  -p:DriverPackageDirectory=C:\payload\driver `
  -p:DriverToolExecutable=C:\payload\SoundSpatializer.DriverCtl.exe

.\installer\wix\Test-PackageFlavor.ps1 `
  -MsiPath .\installer\wix\bin\x64\Production\SoundSpatializer.Production.Installer.msi `
  -ExpectedMode Production
```

Avant diffusion, il faut encore valider ce MSI dans des VM propres: installation,
réparation, upgrade, rollback provoqué, désinstallation, redémarrage requis et
Secure Boot. Le lot actuel ne force pas la terminaison d'un moteur déjà actif:
le retrait PnP et l'éventuel redémarrage demandé par Windows doivent donc être
testés explicitement moteur ouvert, sans recours à `taskkill`. Un arrêt garanti
nécessitera un second canal de contrôle authentifié, car le pipe temps réel est
volontairement mono-client et peut être occupé par l'interface. Le MSI lui-même
doit aussi être signé avec le certificat de
publication; les exécutables Tauri, moteur et DriverCtl doivent être signés avant
assemblage. La présence de ce squelette ne constitue pas une validation HLK ou
une signature Microsoft du pilote.
