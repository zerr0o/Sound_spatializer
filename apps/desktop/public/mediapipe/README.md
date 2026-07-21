# Runtime MediaPipe

Le binaire WASM SIMD est copié ici depuis la version épinglée de
`@mediapipe/tasks-vision` par `npm run assets:sync` et reste ignoré par Git.
Le chargeur ES6 est incorporé au bundle du Web Worker : aucun script de
`public/` n'est importé dynamiquement et rien n'est téléchargé à l'exécution.
