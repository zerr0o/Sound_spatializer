# Validation et critères de sortie

Cette matrice sépare les vérifications automatisables du dépôt des qualifications
qui exigent du matériel, un certificat ou une infrastructure Microsoft.

## Portes automatisées

- schéma `SceneConfigV1` et exemple par défaut ;
- sérialisation/désérialisation exacte du paquet pose 64 octets ;
- signe du yaw et invariance des enceintes dans le monde ;
- impulsions des quatre chemins HRTF, comparaison du chemin partitionné à une
  référence temporelle, longueur maximale et fondu interrompu sans discontinuité ;
- préparation HRTF latest-wins sur un thread distinct, application d'une banque
  de salle prête sans nouvelle requête HRTF et conservation de la durée restante
  d'un fondu de scène ;
- bornes du filtre de pose et retour progressif au neutre ;
- dérive synthétique de l'ASRC et bornage FIFO ;
- sélection de période WASAPI sur multiples fondamentaux et budgets bornés de
  paquets/frames loopback par réveil ;
- validation des valeurs par défaut de route V1, round-trip des deux fournisseurs
  et rejet atomique d'une source absente, inactive ou égale à la sortie ;
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
  sur le matériel de référence ;
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
