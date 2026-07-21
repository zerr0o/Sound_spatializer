# Scientific basis and claim boundaries

Sound Spatializer is a virtual-loudspeaker renderer. It does not try to infer
audio objects from a stereo master. The left input channel feeds the virtual
left loudspeaker and the right input channel feeds the virtual right
loudspeaker. Each loudspeaker contributes to both ears:

```text
earL = inputL * h(Lspeaker -> earL) + inputR * h(Rspeaker -> earL)
earR = inputL * h(Lspeaker -> earR) + inputR * h(Rspeaker -> earR)
```

The filters are evaluated in head-relative coordinates while the loudspeakers
remain fixed in world coordinates.

## Dynamic binaural rendering

- HRTF exchange uses AES69/SOFA `SimpleFreeFieldHRIR`. See the
  [SOFA conventions](https://sofacoustics.org/).
- The optional built-in profiles come from the University of York
  [SADIE II database](https://www.york.ac.uk/sadie-project/database.html),
  licensed under Apache-2.0.
- The five human profiles are not hand-picked. They are seed-42 PAM k-medoids
  over standardized head width, height and depth derived from all 18 public
  SADIE II human OBJ scans. The derived CSV, extraction tool and expected
  medoids are versioned; no scan or ear image is shipped.
- Head movement coupled to binaural rendering can materially improve
  externalization, including for non-individual HRTFs. See Hendrickx et al.,
  [JASA 141, 2011 (2017)](https://doi.org/10.1121/1.4978612).
- A front camera cannot measure the detailed geometry of both pinnae. The app
  therefore offers profile comparison and SOFA import; it never labels face
  tracking as HRTF personalization.
- Abrupt HRIR changes are prohibited. Filter output is morphed during updates,
  and interaural delay is treated separately from the aligned spectral response.

## Tracking and latency

MediaPipe Face Landmarker supplies a facial transform matrix. Inference runs in
a worker because the Web API is synchronous. The native engine filters the
rotation-vector representation with a One Euro Filter and predicts only as far
as the measured render instant, capped at 20 ms.

The professional qualification targets are:

- motion-to-sound p95 at or below 20 ms;
- hard qualification ceiling of 30 ms;
- at least 55 accepted pose updates per second from a requested 60 fps camera;
- wired analogue or USB headphones.

These values are engineering targets, not universal perceptual thresholds.
Published thresholds vary with stimulus and head velocity. See Brungart et al.,
[ICAD 2005](https://www.icad.org/Proceedings/2005/BrungartSimpson2005.pdf).
Software timestamps are insufficient for a release claim; a physical camera to
headphone-output rig is required.

## Rectangular-room auralization

The first room backend is deliberately constrained:

- deterministic image sources for early specular reflections in a rectangular
  room, based on Allen and Berkley,
  [JASA 65, 943 (1979)](https://doi.org/10.1121/1.382599);
- frequency-dependent surface absorption;
- a multiband feedback-delay network for the late diffuse field;
- background rebuilds and crossfades rather than geometry work on the audio
  callback.

Geometrical acoustics is not a reliable prediction of low-frequency modes,
diffraction, or the exact response of real construction assemblies. Material
presets are starting points, not certificates. The UI must use the wording
"perceptually plausible auralization" unless results have been validated against
measured RIR/BRIR data and the relevant ISO 3382 metrics.

## Explicit product limits

- A generic HRTF will not externalize equally well for every listener.
- Headphone response and fit affect the result; EQ profiles remain optional.
- Bluetooth, camera exposure, device drivers, and operating-system periods can
  dominate latency.
- WASAPI loopback covers shared-mode streams sent to the virtual endpoint. ASIO,
  exclusive streams, applications pinned elsewhere, and protected content can
  bypass or reject this path.
- Already-binaural material should use bypass to avoid double spatialization.
