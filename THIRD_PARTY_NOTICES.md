# Third-party notices

Sound Spatializer is designed for commercial-compatible distribution, but a
product build must preserve the notices of every dependency resolved by the
locked JavaScript, Rust and vcpkg manifests. The lockfiles are authoritative;
this file records the principal runtime and data components.

## Runtime software

- MediaPipe Tasks Vision and its Face Landmarker model — Apache License 2.0,
  Google LLC: <https://github.com/google-ai-edge/mediapipe>
- React and React DOM — MIT, Meta Platforms, Inc. and affiliates:
  <https://github.com/facebook/react>
- Three.js — MIT, three.js authors: <https://github.com/mrdoob/three.js>
- Tauri — Apache-2.0 OR MIT, Tauri Programme within The Commons Conservancy:
  <https://github.com/tauri-apps/tauri>
- libmysofa — BSD-3-Clause, Symonics GmbH and Christian Hoene:
  <https://github.com/hoene/libmysofa>
- zlib — zlib License, Jean-loup Gailly and Mark Adler:
  <https://zlib.net/>

The remaining npm and Cargo dependencies are resolved by `pnpm-lock.yaml` and
`apps/desktop/src-tauri/Cargo.lock`. Release packaging must include their full
license texts, not only this index.

## Audio data

The optional built-in SOFA responses are SADIE II v2-2 measurements, Apache
License 2.0, copyright 2018 University of York. Measurements were developed by
Cal Armstrong, Lewis Thresh and Gavin Kearney. See
`resources/hrtf/NOTICE.md` and the pinned record in
`resources/hrtf/profiles.json`.

## Driver and build tooling

- The SysVAD base restored during the driver build is from Microsoft
  `Windows-driver-samples`, MIT. See `native/driver/THIRD_PARTY_NOTICES.md`.
- WiX Toolset 5.0.2 is build-only and uses the Microsoft Reciprocal License.
  WiX binaries are not shipped. See `installer/wix/THIRD_PARTY_NOTICES.md`.

No license for original Sound Spatializer code is granted merely by the
presence of third-party notices. A repository/product license must be selected
by its owner before external distribution.
