# Sound Spatializer

Sound Spatializer est une application Windows 11 qui transforme un mix stéréo
ou PCM 5.1 en enceintes virtuelles fixes dans la pièce. La rotation de la tête, estimée
localement par la webcam, pilote un rendu binaural dynamique vers un casque
filaire ou USB.

Le dépôt contient une implémentation native temps réel, une interface Tauri et
un pilote WaveRT dérivé de SysVAD. Le projet vise une qualification
mouvement-vers-son p95 ≤ 20 ms, mais cette valeur ne peut être revendiquée qu'après
mesure sur le banc physique décrit dans [la procédure de validation](docs/VALIDATION.md).

## Organisation

- `apps/desktop` — Tauri 2, React, TypeScript, Three.js et suivi MediaPipe hors UI.
- `native/engine` — moteur C++20, WASAPI, HRTF, ASRC, acoustique et IPC.
- `native/driver` — surcouche reproductible du sample Microsoft SysVAD/WaveRT.
- `installer/wix` — installateur Windows et cycle de vie du pilote.
- `contracts` — formats persistés et paquets IPC versionnés.
- `resources/hrtf` — manifeste SADIE II et réponses SOFA téléchargées localement.
- `tools` — acquisition vérifiée des ressources et sélection reproductible des profils.

L'[architecture détaillée](docs/ARCHITECTURE.md) décrit les frontières de processus
et les invariants temps réel. Les limites scientifiques et produit sont consignées
dans [SCIENTIFIC_BASIS.md](docs/SCIENTIFIC_BASIS.md), et les changements de repère
sont verrouillés dans [COORDINATES.md](docs/COORDINATES.md).

## Démarrage développeur

Pré-requis principaux : Windows 11 x64, Node.js 22, pnpm 10, Rust stable,
CMake 3.24+, Visual Studio 2022 C++ et WDK 10.0.26100 ou plus récent.

```powershell
pnpm install
pnpm --dir apps/desktop assets:sync
pnpm check
pnpm test
pnpm build
```

Le téléchargement des six HRTF SADIE II est explicite afin de conserver la
traçabilité de leur licence et de leur empreinte :

```powershell
.\tools\fetch-hrtf.ps1
pnpm --dir apps/desktop assets:sync
```

Les dépendances principales et obligations de notice sont recensées dans
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Le propriétaire du projet doit
encore choisir la licence du code original avant toute distribution externe.

L'aperçu web se lance avec `pnpm dev`. Pour une première exécution native (ou
après une modification C++), construisez le moteur Debug complet avec
`pnpm build:engine:debug`, puis lancez le rechargement à chaud avec
`pnpm --dir apps/desktop tauri dev`. En mode Debug, l'hôte préfère ce moteur du
workspace à une ancienne copie éventuellement présente dans `target/debug`.
`pnpm build:ui` produit
le véritable exécutable Tauri Release et vérifie qu'il n'embarque pas l'URL du
serveur de développement. Ne pas le remplacer par un `cargo build --release`,
qui ne sélectionne pas à lui seul le protocole Tauri de production. La
commande place aussi `SoundSpatializer.Engine.exe` dans le même dossier : les
deux exécutables doivent rester côte à côte lorsqu'ils sont copiés sans MSI. La
construction et le test du pilote sont séparés, car ils requièrent Visual
Studio/WDK et, pour l'installation, un environnement de test Windows prévu à cet
effet. Voir [DEVELOPMENT.md](docs/DEVELOPMENT.md).

Pour travailler sans désactiver Secure Boot, l'assistant propose aussi
« Endpoint de rendu externe ». Sélectionnez explicitement l'endpoint render
d'un câble audio virtuel déjà installé et approuvé par Windows, puis un casque
physique différent. Choisissez ensuite cette source comme sortie système dans
les paramètres Son. L'application n'installe aucun composant tiers, ne déduit
jamais un fournisseur depuis son nom et refuse toute route source = sortie. Ce
chemin est un mode de développement/compatibilité ; il ne remplace pas la
qualification du pilote natif et ne doit pas être confondu avec le mode de
tampon audio « Compatibilité » à 256 frames.

L'entrée 5.1 utilise ce chemin externe et exige un endpoint Windows configuré
en six canaux ; le pilote natif reste stéréo. Les cinq canaux principaux sont
spatialisés comme des enceintes L/R/C/LS/RS, tandis que le LFE reste
non directionnel. Voir [la configuration et le test du surround](docs/SURROUND_5_1.md).

Le mode **Fenêtres** est une alternative stéréo : il capture séparément jusqu'à
huit sessions audio, dédupliquées par arbre de processus, et place pour chacune
deux émetteurs L/R sur la largeur de la fenêtre Windows associée. Une ligne
présentée sous le nom d'une application peut donc inclure ses sous-processus
audio. Le mode nécessite le même routage vers un endpoint virtuel afin d'éviter
que le son direct se superpose au rendu binaural. Si une session active ne peut
pas être séparée (son système, neuvième source, capture en échec), le mix global
complet reste audible et la spatialisation par fenêtres attend une couverture
complète. Voir
[la configuration de la spatialisation par fenêtres](docs/WINDOW_SPATIALIZATION.md).

## Sécurité et confidentialité

La vidéo reste dans le processus local, n'est ni enregistrée ni transmise, et
aucun PCM ne traverse le WebView. Les commandes/statuts et les poses utilisent
deux named pipes indépendants, tous deux restreints au SID et à la session
Windows de l'utilisateur. Les profils SOFA
importés sont copiés et hachés avant d'être référencés.

Un pilote test-signé est réservé au développement. Une diffusion publique exige
la signature Microsoft, les essais HLK et la qualification matérielle ; le dépôt
ne tente pas de contourner ces prérequis.
