# Import d’égalisation casque

L’import est entièrement local. L’application n’envoie ni le fichier ni son contenu sur le réseau. Le sélecteur accepte uniquement un fichier local UTF-8 `.json` ou `.txt`, non vide et limité à 1 Mio. Le profil est d’abord prévisualisé ; aucune égalisation importée n’est activée avant le clic explicite sur **Appliquer et activer**.

## JSON Sound Spatializer EQ V1

Toutes les propriétés ci-dessous sont obligatoires et les propriétés inconnues sont refusées.

```json
{
  "format": "sound-spatializer-headphone-eq",
  "schemaVersion": 1,
  "profileName": "Mon casque — mesure 2026",
  "preampDb": -6.0,
  "filters": [
    {
      "enabled": true,
      "type": "peak",
      "frequencyHz": 2800,
      "gainDb": -2.5,
      "q": 1.1
    },
    {
      "enabled": true,
      "type": "high-shelf",
      "frequencyHz": 9000,
      "gainDb": 1.0,
      "q": 0.7
    }
  ]
}
```

Types acceptés : `peak`, `low-shelf` et `high-shelf`. Le profil doit contenir de 1 à 16 filtres, dont au moins un actif. `profileName` contient de 1 à 128 caractères imprimables.

Les nombres non finis sont refusés. Les nombres finis sont bornés avant prévisualisation aux limites du contrat : préampli `[-24, 0]` dB, fréquence `[10, 24000]` Hz, gain `[-24, 24]` dB et Q `[0.01, 30]`.

## Equalizer APO / AutoEQ

Le format texte courant suivant est accepté, sans extension propriétaire :

```text
# Commentaire facultatif
Preamp: -6.5 dB
Filter 1: ON PK Fc 105 Hz Gain 3.2 dB Q 0.70
Filter 2: ON LS Fc 80 Hz Gain -2.0 dB Q 1.00
Filter 3: OFF HS Fc 9000 Hz Gain 1.0 dB Q 0.70
```

- `Preamp` est obligatoire et ne peut apparaître qu’une fois.
- Les numéros de filtre sont positifs et uniques.
- `ON` et `OFF` sont conservés dans la prévisualisation.
- `PK`, `LS`/`LSC` et `HS`/`HSC` sont acceptés.
- Les lignes vides et commentaires commençant par `#` ou `;` sont ignorés.
- Les directives telles que `GraphicEQ`, `Include`, `Copy`, convolution ou chargement de fichier sont refusées.

Cette restriction empêche qu’un profil texte déclenche une lecture secondaire, une inclusion ou une autre action hors du fichier explicitement choisi.
