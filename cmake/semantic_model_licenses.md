# Semantic runtime provenance and license notices

The native library is extracted without modification from the official
`mediapipe-1.0.0-py3-none-win_amd64.whl`. Its package metadata declares Apache 2.0.
The complete wheel `LICENSE` and third-party `NOTICE` are installed alongside the
library, byte for byte. The C headers come from MediaPipe tag `v1.0.0`, commit
`6d31f1ebc3284db74d211d62bdc4f0a0c29ea120`; their Apache license and origin are
retained in the source vendor directory.

The two model files are unmodified, versioned downloads linked by the official
MediaPipe Tasks documentation. Exact URLs, byte sizes and SHA256 digests are in
`runtime-manifest.json`.

* Body model: `selfie_multiclass_256x256`, float32, version 1. Official description:
  <https://ai.google.dev/edge/mediapipe/solutions/vision/image_segmenter>.
  Model card: <https://storage.googleapis.com/mediapipe-assets/Model%20Card%20Multiclass%20Segmentation.pdf>.
* Face model bundle: `face_landmarker`, float16, version 1. Official description:
  <https://ai.google.dev/edge/mediapipe/solutions/vision/face_landmarker>.
  The bundle contains the face detector, face landmark detector, blendshape model
  and face geometry metadata; blendshape output is disabled in this adapter.
  Individual model cards:
  <https://storage.googleapis.com/mediapipe-assets/MediaPipe%20BlazeFace%20Model%20Card%20%28Short%20Range%29.pdf>,
  <https://storage.googleapis.com/mediapipe-assets/Model%20Card%20MediaPipe%20Face%20Mesh%20V2.pdf>,
  <https://storage.googleapis.com/mediapipe-assets/Model%20Card%20Blendshape%20V2.pdf>.

The four official model cards listed above each explicitly state that the model
is licensed under Apache License 2.0 (verified from the PDFs on 2026-09-15). This
license identification is based on the model cards themselves, not the separate
documentation footer that licenses page content and sample code. The downloaded
weights do not embed an additional license text; retain this provenance record
and the complete Apache license distributed alongside them.
