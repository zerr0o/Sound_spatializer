# Architecture

## Chemin critique

```text
Applications Windows (mix partagé stéréo, 48 kHz float32)
                    │
                    ▼
Endpoint render source
  ├─ WaveRT « Sound Spatializer » (préféré)
  └─ endpoint tiers choisi explicitement (compatibilité non qualifiée)
                    │ WASAPI loopback event-driven
                    ▼
SoundSpatializer.Engine.exe
  FIFO bornée → ASRC polyphasé → HRTF 2×2 → pièce/EQ/limiteur
                    │ IAudioClient3 partagé ou WASAPI exclusif
                    ▼
Casque physique explicitement sélectionné

Webcam → Worker MediaPipe → quaternion horodaté ──pipe Pose dédié──┘
Tauri/React ──commandes et télémétrie JSON──pipe Engine duplex────┘
```

Le mode stéréo par fenêtres remplace, à l'intérieur du moteur, le lit L/R par
jusqu'à huit captures process-loopback indépendantes. Chaque entrée part d'une
session audio, puis les sessions d'un même PID et les descendants déjà couverts
par un parent sont dédupliqués en arbres de processus. Un arbre fournit deux
sources L/R dont la position provient de sa fenêtre associée et du plan d'écran
Windows, soit une matrice HRTF dynamique maximale 16×2. Le renderer
physique et l'endpoint source restent les mêmes ; le mix global capturé par le
backend sert encore d'horloge et de repli. Le passage aux processus est
atomique : toutes les racines de sessions actives observées doivent avoir un
slot prêt. Une session système, exclue, désactivée, non assignée ou en échec
maintient donc le mix endpoint complet ; aucun sous-ensemble process n'est rendu
seul. Une fois la couverture complète, l'endpoint est exclu et n'est jamais
additionné au rendu par processus. Deux indices HRTF supplémentaires restent
réservés au repli endpoint L/R dans les banques par fenêtres ; cette paire
dormante rend les transitions asynchrones audibles sans réutiliser les seize
indices stables des applications.

La découverte des sessions utilise l'identifiant canonique de l'endpoint
effectivement résolu par le backend WASAPI après sélection par marqueur ou ID
explicite. Elle ne retombe donc jamais implicitement sur la sortie Windows par
défaut lorsque le pilote natif est choisi. Chaque slot process-loopback conserve
également sa paire fixe d'indices HRTF ; la disparition d'un slot inférieur ne
remappe pas transitoirement les applications restantes sur d'anciens filtres.

L'endpoint virtuel est une source pour le moteur, jamais une destination. Le
moteur rejette son GUID stable lors de l'énumération des sorties afin d'empêcher
une boucle audio, indépendamment du nom affiché ou de la langue de Windows.

### Routage de capture

Le fournisseur `native-driver` découvre exclusivement l'endpoint Sound
Spatializer grâce à son marqueur fournisseur et à sa version de contrat. Le
fournisseur `external-render` ouvre en loopback un endpoint render WASAPI déjà
installé et choisi explicitement par son identifiant; aucun nom affiché ou motif
propre à un fournisseur ne sert d'auto-détection. Le projet ne télécharge,
n'installe ni ne redistribue de pilote audio tiers.

La commande `set-audio-route` valide puis applique atomiquement le fournisseur,
l'endpoint source et le casque physique. La source ne peut jamais être la sortie
du moteur; une route absente, inactive ou rebouclée est rejetée sans bascule
silencieuse et sans écriture partielle. Le mode externe est présenté comme une
compatibilité non qualifiée tant qu'il n'a pas satisfait la même campagne de
latence, stabilité et dérive que le pilote natif.

Le choix de route ne doit pas être confondu avec `audio.mode=compatibility` : ce
dernier sélectionne seulement une cible de tampon de 256 frames, quel que soit
le fournisseur de capture.

## Frontières de processus

### Interface Tauri

L'interface configure la scène, initialise la caméra et présente les diagnostics.
`FaceLandmarker.detectForVideo()` s'exécute dans un Web Worker. Le chemin préféré
utilise `MediaStreamTrackProcessor`; le repli transfère des `ImageBitmap` produits
par `requestVideoFrameCallback`. Le modèle et le runtime WASM sont embarqués et
vérifiés par empreinte. Le chargeur ES6 MediaPipe est incorporé au bundle du
Worker ; aucun script n'est importé dynamiquement depuis `public/`.

Tauri ne traite jamais le signal audio. Il supervise un moteur autonome et peut
être fermé ou minimisé sans interrompre le rendu.

### Moteur natif

Le moteur possède les clients WASAPI, l'horloge audio, tous les états DSP et les
files SPSC. L'interpolation des HRIR et la projection de pièce sont préparées
hors callback puis publiées par échange atomique et fondu. La préparation des
spectres partitionnés et du cache de transition reste une limite temps réel
explicitement suivie dans `VALIDATION.md`. Le tracking est filtré dans l'espace
du vecteur de rotation et prédit au plus 20 ms vers l'instant de rendu.

La découverte des fenêtres et sessions, les appels COM/DWM et l'activation des
captures par processus s'exécutent sur des threads de contrôle. Chaque session
alimente un ASRC stéréo préalloué ; le callback ne voit que des handles
générationnels, un snapshot atomique et des positions atomiques.

Le callback MMCSS respecte les invariants suivants :

- aucune allocation/libération ;
- aucune attente, I/O, journalisation ou acquisition de mutex ;
- mémoire et filtres préalloués ;
- travail borné par bloc ;
- télémétrie écrite dans une file lock-free et consommée ailleurs.

### Pilote virtuel

Le pilote est une surcouche minimale et auditée du sample Microsoft SysVAD. Il
n'expose qu'un endpoint de rendu stéréo 48 kHz float32 et son chemin loopback.
Le tampon de rendu est recopié dans un historique non paginé borné ; le loopback
lit cet historique. Aucun générateur de tonalité du sample n'appartient au chemin
produit.

## Contrats

Le schéma persistant canonique de scène est
`contracts/scene-config-v2.schema.json`. Le mode dynamique utilise séparément
`contracts/window-spatialization-v1.schema.json`. Les
configurations V1 sont relues puis migrées sans écraser leur fichier source. Les
types plus riches de l'interface doivent être convertis explicitement à cette
forme ; ils ne constituent pas un second format implicite.

La pose emprunte un paquet binaire fixe de 64 octets sur un pipe entrant dédié,
indépendant du pipe duplex des commandes et statuts JSON. Les JSON UTF-8 sont
précédés d'une longueur, avec une limite stricte. Les deux pipes sont versionnés,
suffixés par le SID et l'identifiant de session Windows, créés en instance unique
et protégés par une ACL pour ce SID. Les migrations de
configuration sont monotones et atomiques (`temp` + remplacement).

Chaque commande JSON porte un identifiant injecté par l'hôte Tauri. Le moteur
répond sur le pipe duplex par un ACK/NACK portant le même identifiant ; une
écriture réussie dans le pipe n'est donc jamais confondue avec l'application
effective d'une commande. La réponse distingue l'application en mémoire de la
persistance atomique. Les attentes sont rattachées à une génération de
connexion : la fermeture tardive d'un ancien lecteur ne peut pas invalider les
commandes déjà envoyées sur le pipe reconnecté.

## Repère spatial

Le monde est direct : X vers la droite, Y vers le haut, Z vers l'avant. Le sol
est Y=0. Un quaternion `(w,x,y,z)` transforme la tête locale vers le monde ; le
moteur applique son inverse pour exprimer une source fixe dans le repère tête.

Les surfaces de pièce sont toujours ordonnées `[-X,+X,-Z,+Z,-Y,+Y]`, où `-Y`
est le sol et `+Y` le plafond.
