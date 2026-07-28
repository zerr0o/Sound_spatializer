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

## Two corrections the renderer applies to itself

Rendering that way introduces two colourations that are properties of the
method, not of the programme material. Both are measured by the engine test
suite rather than assumed, and the measured values are quoted below.

### Provider neutralization

A SOFA file carries no absolute level convention, and libmysofa normalizes on
whichever measurement minimizes azimuth plus elevation, which on a set with a
bottom pole is not the frontal response. The shipped SADIE II profiles come out
of that about 10 dB hot: a correlated signal through the default loudspeaker
pair measures +15.6 dB of broadband gain. With the default -6 dB master gain and
the -0.1 dBFS ceiling, the true-peak limiter then sits in permanent deep gain
reduction, which is audible as hardness well before any spectral tilt is.

At load time the engine power-averages every response in the set, smooths the
result to a third of an octave and designs the minimum-phase inverse. The level
part of that inverse applies at all frequencies; only the spectral part is
tapered out below 300 Hz and above 15 kHz, where inverting a measurement would
amplify noise. Because the corrected component is direction-independent by
construction, one stereo FIR on the binaural bus is equivalent to correcting all
thirty-six paths. The same pair then measures +5.5 dB, which is the coherent
summation a real loudspeaker pair produces.

Measurement also settles a question worth recording: the shipped SADIE II sets
are **already** diffuse-field equalized. Their direction-averaged magnitude is
flat within about 1 dB from 200 Hz to 12 kHz, so their correction is a nearly
flat -10 dB attenuation and nothing more. An imported set that is not equalized
receives the spectral part as well.

### Phantom-centre compensation

Both emitters of a pair reach both ears, so anything correlated between them
arrives twice with the delay that separates them. The resulting comb is deep and
its frequency follows the emitter angle, which is what makes a window that is
moved or resized change timbre. Measured against a third-octave mean, the
uncompensated ripple is 10.9 dB at ±30°, 9.0 dB at ±22°, 10.1 dB at ±15° and
7.3 dB at ±8°.

The renderer keeps both emitters and rewrites their filters so that only the mid
component is corrected:

```text
H'left  = [ C*(Hleft + Hright) + (Hleft - Hright) ] / 2
H'right = [ C*(Hleft + Hright) - (Hleft - Hright) ] / 2
```

`C` is derived from the interference factor `|Hleft + Hright|` divided by the
power sum, a ratio that cancels the HRTFs' own magnitude structure so smoothing
it flattens the comb without touching the pinna notches that carry elevation
cues. The rewrite is algebraically `mid * C * (Hleft + Hright) + side * (Hleft -
Hright)`, so the real-time convolver, its morphing and the sparse window slots
are unchanged. Residual ripple is under 3 dB at every geometry above.

The correction is not free and the product must not pretend otherwise: a
hard-panned source has a mid component too, so it shifts by up to 4.9 dB at the
notch frequencies even though it never combed. The boost is capped at +10 dB to
bound that trade, and the whole correction is switchable so it can be compared
by ear.

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
- Abrupt HRIR changes are prohibited. Filter output is morphed during updates.
- The loader re-applies any per-measurement delay the file declares, but the
  shipped SADIE sets declare none, so on the direct path libmysofa interpolates
  full-phase responses between neighbours. Separating interaural delay from an
  aligned spectral response is not implemented there yet; the reflected field
  handled by the order-3 decoder is the only path that does it.

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
