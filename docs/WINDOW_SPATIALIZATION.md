# Spatialisation stéréo par fenêtres

Le mode **Fenêtres** sépare le son à partir des sessions audio Windows et
conserve chaque paire stéréo. Les sessions d'un même PID et les processus
descendants déjà couverts par leur parent sont regroupés en une capture d'arbre
de processus. Pour chaque capture retenue, le canal gauche et le canal droit
deviennent deux sources HRTF distinctes, placées sur la largeur de la fenêtre
associée. Le nom d'application affiché est donc un libellé pratique, pas la
garantie d'une correspondance un-pour-un. La rotation de tête continue à
stabiliser ces deux émetteurs dans le monde, comme pour les enceintes virtuelles
de la scène classique.

Ce mode ne prétend pas localiser un objet sonore à l'intérieur d'une vidéo ou
d'un jeu. Windows permet d'associer une session audio à un processus et ce
processus à une fenêtre ; il ne fournit pas la position visuelle de chaque son
au sein du contenu.

## Routage requis

1. Dans l'assistant, choisir un endpoint de rendu virtuel comme **source** et un
   casque physique distinct comme **sortie**.
2. Dans Windows, envoyer les applications à spatialiser vers cet endpoint
   virtuel, globalement ou avec le mélangeur de volume par application.
3. Dans la vue Scène, sélectionner **Fenêtres**.
4. Lancer du son dans une application possédant une fenêtre visible.

Le son original ne doit pas être envoyé en parallèle directement au casque :
il se superposerait alors au rendu binaural. La protection source = sortie du
moteur reste active et empêche une boucle audio.

Le mode est strictement stéréo. Une scène 5.1 doit être repassée en **Stéréo**
avant son activation. La pièce hybride est désactivée dans ce mode initial afin
de ne pas mélanger les réflexions du lit d'enceintes avec des sources
dynamiques.

## Chemin natif

- `IAudioSessionManager2` découvre les sessions actives et pré-arme les sessions
  inactives non expirées sur l'endpoint source afin de ne pas manquer les sons
  courts. Un flux pré-armé n'est toutefois déclaré audible qu'après passage de
  la session de cet endpoint à l'état actif et réception d'un paquet PCM non
  silencieux. Le PCM seul ne peut jamais promouvoir une session : l'API
  process-loopback suit un processus sur toutes ses sorties et provoquerait
  sinon un doublage lorsqu'une application est reroutée directement vers le
  casque. Un paquet non silencieux reçu pendant que la session source est encore
  inactive révoque atomiquement la couverture et rétablit le mix endpoint dès
  le callback qui observe ce veto, sans jamais autoriser ce PCM process. Si la
  session devient active sur l'endpoint source, la découverte réinitialise son
  tampon et attend un nouveau bloc complet. Si elle reste inactive, le veto
  n'est relâché qu'après 150 ms sans nouveau PCM afin d'éviter le flapping
  produit par un flux émis sur un autre endpoint. La découverte nominale est
  interrogée toutes les 10 ms.
- La capture process-loopback Windows inclut l'arbre de processus ciblé, ce qui
  évite de perdre les sous-processus audio des navigateurs et lecteurs.
- Le PID est associé à sa plus grande fenêtre de premier niveau non masquée,
  puis au moniteur Windows le plus proche.
- Jusqu'à huit sessions/arborescences de processus dédupliquées sont capturées
  simultanément, soit seize chemins source→oreille dans la matrice HRTF.
- Chaque capture, inscrite MMCSS `Pro Audio` (repli `Audio`), alimente un ASRC
  polyphasé et une FIFO préalloués. Le callback audio ne réalise ni allocation,
  ni appel COM, ni attente, ni mutex.
- Les positions L/R, gains et filtres changent par morphing ; aucun PCM ne
  traverse Tauri.
- Les indices HRTF sont attachés aux slots générationnels de capture : retirer
  une session/arborescence ne décale pas les paires L/R des autres captures.
- Le passage process-loopback est **tout ou rien**. Tant qu'une session active
  observée est système, exclue, désactivée, non assignée, en échec ou encore
  non prête, le mix endpoint complet reste l'unique chemin audible. Les FIFO
  process continuent d'être drainées et amorcées en arrière-plan. Le mix
  endpoint n'est exclu qu'une fois toutes les racines actives couvertes, ce qui
  évite de rendre une application localisée en coupant silencieusement les
  autres.

Ce repli concerne la source directe uniquement : tant que le mode Fenêtres est
demandé, l'acoustique de pièce et l'heuristique « déjà binaural » restent
désactivées comme pour le chemin process-loopback. Une activation tardive ne
peut ainsi pas changer deux fois la balance sonore.

L'agencement est relu périodiquement. Une fenêtre déplacée d'un écran à l'autre
fait donc glisser sa paire L/R vers le nouveau plan d'écran sans modifier les
canaux. Les changements de gain, largeur stéréo, règle et calibration sont
appliqués à chaud sans recréer les flux.

## Calibration et règles

L'onglet **Écrans** permet de remplacer la projection automatique en pixels par
les dimensions, la position et l'angle horizontal physiques mesurés de chaque écran. L'identifiant
persisté est le chemin de moniteur retourné par Windows, pas son nom affiché.

L'onglet **Sources** permet, par identifiant d'application associé à une
session/arborescence :

- d'activer ou couper sa spatialisation ;
- de régler son gain ;
- de choisir l'écartement L/R ;
- de définir un écran de repli lorsqu'aucune fenêtre ne peut être trouvée.
- de supprimer entièrement une règle mémorisée devenue inutile.

Le catalogue accepte au maximum 64 règles persistées. L'interface refuse
explicitement une règle supplémentaire au lieu de la tronquer silencieusement.
La largeur stéréo initiale canonique de l'interface et du contrat d'exemple est
`0,72` ; une règle peut ensuite la remplacer entre mono centré (`0`) et largeur
complète de la fenêtre (`1`).

Les PID, HWND, titres et états de capture restent de la télémétrie volatile.
Seuls les identifiants d'application, calibrations et réglages sont persistés
dans `window-spatialization-v1.json`.

## Limites de la première version

- Windows 11 est la cible ; l'API process-loopback requiert au minimum le build
  Windows 10 20348.
- Les flux ASIO, WASAPI exclusifs, protégés ou sans session partagée compatible
  peuvent ne pas être capturables.
- Une arborescence possédant plusieurs fenêtres utilise actuellement la plus
  grande fenêtre visible de ses processus.
- Une session sans fenêtre exploitable est placée sur l'écran de repli, ou
  sur l'écran principal.
- Les sons système sans processus ciblable et les sessions/arborescences
  actives au-delà de la limite de huit ne peuvent pas être séparés. Leur
  présence suspend donc temporairement la spatialisation par fenêtres et
  conserve le mix endpoint complet, sans les rendre muets.
- Une session préarmée rétablit immédiatement le mix endpoint dès son premier
  PCM non silencieux, avant le poll suivant. Un processus réellement nouveau,
  qui ne possède encore aucun slot process-loopback, dépend encore du poll de
  10 ms et peut donc perdre au pire ce très court intervalle si un autre
  ensemble process est déjà rendu. Des notifications
  `IAudioSessionNotification` / `IAudioSessionEvents` restent l'extension
  nécessaire pour une garantie exacte à l'échantillon.
- La capture process-loopback couvre un arbre de processus sur tous ses
  endpoints. Si le même arbre diffuse simultanément un flux vers la source
  virtuelle et un autre directement vers le casque, Windows ne fournit pas ici
  de soustraction par session : un doublage reste possible. Éviter ce routage
  mixte pour un même processus.
- La qualification finale de latence et de dérive exige encore le banc physique
  et le soak de 24 h décrits dans `VALIDATION.md`.
