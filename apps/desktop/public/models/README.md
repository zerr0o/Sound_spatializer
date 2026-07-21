# Modèle de suivi facial

Lancer `npm run assets:sync` depuis `apps/desktop` après l'installation des dépendances.
Le script copie le runtime WASM de `@mediapipe/tasks-vision` et télécharge le modèle
officiel `face_landmarker.task`. Ces fichiers générés sont ignorés par Git afin de ne
pas republier des binaires tiers ; le paquet final Tauri doit inclure le dossier `public`.
