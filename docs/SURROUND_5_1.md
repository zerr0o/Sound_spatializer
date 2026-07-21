# Entrée surround 5.1

La branche `feature/5-1-surround` prend en charge une entrée PCM 5.1 provenant
d'un endpoint de rendu externe. La sortie du moteur reste binaurale stéréo pour
le casque.

## Routage recommandé avec VB-CABLE

1. Ouvrir `mmsys.cpl`.
2. Dans **Lecture**, réactiver au besoin les périphériques désactivés.
3. Sélectionner **CABLE Input (VB-Audio Virtual Cable)**, puis
   **Configurer > Surround 5.1**.
4. Régler son format sur 48 kHz dans **Propriétés > Avancé**.
5. Choisir **CABLE Input** comme sortie Windows par défaut, puis redémarrer les
   applications qui doivent produire du 5.1.
6. Dans Sound Spatializer, choisir le fournisseur **Endpoint externe**,
   **CABLE Input** comme source et le casque physique comme sortie.
7. Dans la scène, choisir **Entrée 5.1**.

Le nom de VB-CABLE est contre-intuitif : `CABLE Input` est bien son endpoint de
lecture, celui vers lequel les applications Windows envoient leur audio.

## Contrat de canaux

Le moteur exige six canaux WAVEFORMATEXTENSIBLE dans l'ordre Windows :

| Index | Canal PCM | Source virtuelle |
| ---: | --- | --- |
| 0 | FL | Enceinte avant gauche |
| 1 | FR | Enceinte avant droite |
| 2 | FC | Enceinte centrale |
| 3 | LFE | Grave non directionnel |
| 4 | SL ou BL | Enceinte surround gauche |
| 5 | SR ou BR | Enceinte surround droite |

Les masques `0x3F` (5.1 historique avec BL/BR) et `0x60F` (5.1 surround avec
SL/SR) sont acceptés. Tout autre masque, tout flux 7.1/16 canaux et tout endpoint
encore configuré en stéréo sont refusés explicitement. Windows ne doit jamais
matricer une source stéréo en six canaux à l'insu de l'utilisateur.

Les cinq canaux principaux disposent chacun d'une position et d'une HRTF. Le
LFE n'est pas affiché comme une enceinte localisable : il est filtré à 120 Hz,
distribué de manière identique aux deux oreilles et exclu des réflexions de
pièce.

## Vérification

Le diagnostic doit indiquer :

- `captureChannels = 6` ;
- `captureChannelMask = 0x3F` ou `0x60F` ;
- `inputLayout = 5.1-surround` ;
- une sortie physique distincte de la source VB-CABLE.

Tester ensuite six impulsions ou tonalités distinctes, une par canal. Les cinq
canaux principaux doivent apparaître aux positions correspondantes ; le LFE
doit rester centré et invariant lorsque la tête tourne.

## Limites de cette première version

- Le pilote natif Sound Spatializer reste stéréo et n'est pas utilisé pour ce
  chemin 5.1.
- Le 7.1, Dolby Atmos, DTS et les objets audio spatiaux ne sont pas décodés.
- Les applications déjà ouvertes peuvent conserver leur ancien format stéréo ;
  elles doivent être relancées après la configuration de l'endpoint.
- Le contenu protégé, ASIO et les flux exclusifs qui contournent le mixeur
  Windows restent hors périmètre.
