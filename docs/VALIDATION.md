# Validation et critères de sortie

Cette matrice sépare les vérifications automatisables du dépôt des qualifications
qui exigent du matériel, un certificat ou une infrastructure Microsoft.

## Portes automatisées

- schémas `SceneConfigV1`/`SceneConfigV2`, exemples et migration V1→V2 ;
- sérialisation/désérialisation exacte du paquet pose 64 octets ;
- signe du yaw et invariance des enceintes dans le monde ;
- impulsions des dix chemins HRTF en 5.1 (et quatre en stéréo), comparaison du chemin partitionné à une
  référence temporelle, longueur maximale et fondu interrompu sans discontinuité ;
- préparation HRTF latest-wins sur un thread distinct, application d'une banque
  de salle prête sans nouvelle requête HRTF et conservation de la durée restante
  d'un fondu de scène ;
- bornes du filtre de pose et retour progressif au neutre ;
- dérive synthétique de l'ASRC et bornage FIFO ;
- sélection de période WASAPI sur multiples fondamentaux et budgets bornés de
  paquets/frames loopback par réveil ;
- validation des valeurs par défaut de route V2, round-trip des deux fournisseurs
  et rejet atomique d'une source absente, inactive ou égale à la sortie ;
- validation stricte de l'entrée 5.1 (six canaux, masque `0x3F` ou `0x60F`),
  isolation FL/FR/FC/SL/SR et rendu LFE symétrique filtré à 120 Hz ;
- RIR des sources-images contre positions analytiques ;
- rejet des SOFA incompatibles ;
- type-check, tests et build production de l'interface ;
- build MSVC du moteur et tests CTest ;
- inspection structurelle du package pilote.

## Banc de référence

Configuration : Windows 11 24H2, 48 kHz/128 frames, webcam USB filaire 60 fps,
casque USB ou filaire. Un actionneur ou plateau produit un mouvement de tête connu ;
une LED synchronisée marque l'image caméra et une acquisition externe mesure le
changement du signal casque. Les horodatages logiciels ne sont utilisés que pour
la décomposition, jamais comme preuve finale de mouvement-vers-son.

Critères bloquants :

- mouvement→PCM p95 ≤ 20 ms, refus au-delà de 30 ms ;
- pipeline audio ajouté ≤ 20 ms ;
- au moins 55 poses/s acceptées ;
- callback audio p99 < 50 % de sa période ;
- zéro dropout, underrun et overrun sur 24 h ;
- FIFO bornée sous dérive injectée ±1000 ppm ;
- perte/reprise caméra et hotplug casque sans clic.

Une webcam 30 fps ou une sortie Bluetooth doit être clairement classée en mode
dégradé et ne peut pas satisfaire le profil professionnel.

## Pilote et système

- Driver Verifier sans violation ;
- playlist HLK pertinente réussie ;
- Secure Boot et package Microsoft-signé pour la release publique ;
- installation, mise à niveau, rollback et désinstallation propres ;
- veille, hibernation, reprise et changement d'alimentation ;
- applications WASAPI partagées, changements de format et contenus protégés ;
- impossibilité de choisir l'endpoint virtuel comme sortie du moteur.

## Endpoint render tiers

Le fournisseur `external-render` est une compatibilité non qualifiée par défaut.
Il n'est jamais découvert par nom et doit être exercé avec l'identifiant WASAPI
explicitement sélectionné. La campagne couvre au minimum :

- validation et persistance atomiques de la source, de la sortie et du
  fournisseur, sans bascule silencieuse vers le périphérique par défaut ;
- refus source=sortie, disparition et réactivation de la source, changement
  d'identifiant après mise à jour, redémarrage du service audio et veille/reprise ;
- formats annoncés, périodes réellement obtenues, ordre L/R, silence, contenus
  WASAPI partagés et changements de format ;
- hotplug du casque, dérive d'horloges, FIFO bornée, compteurs d'xruns et soak de
  24 heures sans dropout ;
- mesure physique mouvement→casque avec les mêmes seuils p95 et plafond que le
  chemin natif.

Si un fournisseur ou une version ne satisfait pas l'ensemble de ces portes, son
usage reste explicitement dégradé et ne peut pas hériter de la qualification
professionnelle du pilote natif. Revendiquer une compatibilité générique exige
une matrice reproductible de plusieurs pilotes et versions; sinon la liste de
support doit nommer exactement les produits qualifiés. Le programme de test
n'installe et ne redistribue aucun composant tiers.

## Sessions multiples et process-loopback

Le mode Fenêtres doit être validé comme une banque de sessions/arborescences de
processus, et non comme une correspondance supposée un-pour-un entre application,
fenêtre et flux audio. Les tests automatisés et les essais Windows couvrent :

- l'isolation impulsionnelle des canaux L/R de chaque capture, la duplication
  explicite d'une source mono et l'absence de contamination entre deux arbres de
  processus simultanés ;
- la déduplication de plusieurs sessions d'un même PID et d'un processus enfant
  déjà inclus par la capture de son parent, notamment avec navigateurs,
  lecteurs multiprocessus et deux instances d'un même exécutable ;
- le pré-armement d'une session inactive : le mix endpoint reste seul audible
  jusqu'à ce que la session soit active et qu'un PCM non silencieux soit reçu ;
  le passage au rendu process-loopback puis le retour au repli ne créent ni
  silence, ni doublage, ni clic. Un PCM capturé pendant que la session de
  l'endpoint source est inactive ne doit jamais l'activer : il est drainé et
  ignoré afin qu'une application reroutée vers le casque ne soit pas rendue une
  seconde fois ;
- la priorité d'une session active sur un slot inactif, la limite explicite de
  huit captures, et la stabilité des paires HRTF lorsqu'une capture intermédiaire
  disparaît ou redémarre ;
- le repli tout-ou-rien : son système actif, PID non ciblable, règle désactivée,
  neuvième racine active, capture non prête ou échec d'énumération doivent
  conserver le mix endpoint complet. Les autres FIFO continuent d'être vidées,
  mais aucun sous-ensemble process ne devient audible ;
- la sélection de la plus grande fenêtre visible d'une arborescence, les
  fenêtres déplacées, réduites ou recréées, les coordonnées d'écran négatives,
  les DPI mixtes, rotations, hotplug moniteur et calibrations persistées ;
- le déplacement à chaud d'une fenêtre entre écrans sans recréer sa capture et
  sans permutation L/R, ainsi que le repli déterministe sur l'écran configuré
  lorsqu'aucune fenêtre n'est exploitable ;
- le reroutage d'une session hors de l'endpoint source, sa réapparition, le
  redémarrage du service audio et le maintien de la protection contre tout rendu
  simultané endpoint/process-loopback ;
- les compteurs distincts d'overrun et d'underrun des FIFO de capture, une dérive
  injectée de ±1000 ppm, un soak de 24 h et une charge de huit captures stéréo
  (seize sources applicatives HRTF plus la paire endpoint dormante), avec
  callback p99 sous 50 % de sa période et zéro interruption.

Le banc mesure aussi le délai d'activation d'un son court depuis l'état inactif.
Sur une session préarmée, le premier PCM non silencieux doit révoquer
atomiquement la couverture, rétablir le mix endpoint sans attendre un nouveau
poll et ne jamais rendre ce PCM process tant que l'endpoint source ne l'a pas
déclaré actif. Si la session reste inactive, le veto n'est relâché qu'après
150 ms sans nouvelle révocation. Pour un processus réellement nouveau sans
slot préarmé, le poll est réglé à 10 ms : il
faut mesurer séparément l'attaque résiduelle d'au plus un intervalle, la
transition vers la première paire L/R et la latence mouvement→casque. Une
garantie exacte à l'échantillon exige encore les notifications de session
Windows ; des timestamps logiciels seuls ne qualifient aucun de ces résultats.

## Écoute

Le protocole en aveugle compare pose statique/dynamique et plusieurs HRTF. Les
mesures sont externalisation, stabilité des enceintes, localisation angulaire et
confusions avant/arrière. Les résultats sont publiés avec taille d'échantillon et
incertitude ; aucune formulation ne promet une perception identique pour tous.

## État d'une construction locale

Une construction locale peut démontrer le code, les tests et la latence logicielle.
Elle ne devient « qualifiée professionnelle » qu'après réussite documentée de
toutes les portes matérielles, HLK et signature ci-dessus.

Limites d'implémentation encore ouvertes dans cette révision :

- le rendu L/R direct conserve 512 taps temporels sans latence et traite la
  queue par partitions de 128 frames jusqu'à 4 096 taps ; les SOFA plus longs
  sont rejetés. Cette borne et le coût maximal doivent encore être qualifiés
  sur le matériel de référence. L'interpolation des HRIR est exécutée par le
  worker, mais la construction des spectres partitionnés et le recalcul du
  cache de transition sont encore déclenchés par `set_filters()` dans le
  callback. Leur déplacement sûr hors temps réel, avec préchauffage de la queue
  partitionnée, reste une porte avant qualification des HRTF de plus de
  512 taps ;
- le décodeur de pièce utilise bien des FIR HRTF 16×2 préalloués, mais son pire
  cas à 512 taps, la troncature documentée de la queue HRTF pour le seul champ
  réfléchi et la copie/application des banques préparées doivent être qualifiés
  sur le matériel de référence ; les requêtes et la projection sont hors callback ;
- la reprise WASAPI après invalidation est automatique et bornée, sans toutefois
  remplacer les essais physiques de période minimale loopback, repli
  `IAudioClient::Initialize`, priorité rendu sous backlog, hotplug, veille/reprise
  et disparition du périphérique ; le worker x64 configure FTZ/DAZ, mais les
  mesures p99 restent une porte matérielle ;
- la compilation WDK, Driver Verifier, HLK, la signature Microsoft et le banc
  mouvement→sortie casque restent des portes externes à cette construction.
