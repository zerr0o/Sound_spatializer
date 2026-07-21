# Repères et conventions de rotation

Les conversions ci-dessous sont contractuelles. Elles évitent les erreurs de
signe qui donnent une scène semblant tourner avec la tête.

## Monde Sound Spatializer

- `+X` : droite de l'auditeur ;
- `+Y` : haut ;
- `+Z` : avant, vers l'écran/la webcam ;
- quaternion `(w,x,y,z)` : tête locale vers monde calibré ;
- yaw positif : l'auditeur tourne le visage vers sa droite.

Pour interroger une HRTF, une direction monde fixe est transformée vers la tête
par l'inverse du quaternion. Une enceinte devant doit donc partir vers la gauche
dans le repère tête lorsque l'auditeur tourne la tête à droite.

La vitesse angulaire du paquet pose est exprimée dans le monde calibré, en rad/s.
À partir de deux poses, le delta monde est `q_current * inverse(q_previous)` ; une
prédiction à vitesse constante se pré-multiplie à la pose courante.

## SOFA / libmysofa

Le repère SOFA usuel est `X` avant, `Y` gauche, `Z` haut. Une direction normalisée
Sound Spatializer est donc passée à libmysofa ainsi :

```text
sofa = { x: world.z, y: -world.x, z: world.y }
```

Les délais renvoyés par `mysofa_getfilter_float` sont des secondes. Ils doivent
être convertis en échantillons, sans perdre leur partie fractionnaire, avant de
réintroduire l'ITD.

## MediaPipe Face Transform

La matrice faciale est column-major et transforme le visage canonique vers le
visage détecté. Contrairement aux landmarks écran, son espace métrique est droit,
Y est dirigé vers le haut et la caméra regarde vers `-Z`.

La vidéo donnée au modèle n'est pas retournée horizontalement. Pour une webcam
placée face à l'auditeur, la base caméra et la base auditeur diffèrent par une
réflexion de X. Pour une rotation relative, la conversion quaternion correspond
à :

```text
q_audio = { w: q_camera.w, x: q_camera.x, y: -q_camera.y, z: -q_camera.z }
```

Le rendu de prévisualisation peut être miroir en CSS pour le confort sans changer
les pixels fournis au modèle. Toute option qui retourne réellement l'image doit
modifier explicitement cette conversion.

La calibration mémorise une référence et calcule
`inverse(q_reference) * q_current`, donnant une pose tête-locale → monde neutre.

## Tests de signe minimaux

- identité : enceintes à -30°/+30° ;
- tête à +30° yaw : azimuts tête des enceintes -60°/0° ;
- tête à -30° yaw : azimuts tête 0°/+60° ;
- `q` et `-q` : même pose et vitesse nulle ;
- source monde +X : côté droit SOFA (`Y < 0`) ;
- axes avant/droite/haut : coefficients ambisoniques conformes au mapping Y-up.
