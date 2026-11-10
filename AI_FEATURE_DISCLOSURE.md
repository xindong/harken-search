# AI Feature Disclosure

> **法务占位：AI 功能披露正文为 PLACEHOLDER，最终措辞待法务确认。以下事实已按当前源码核实。**

This note records the AI-related surfaces currently visible in the Harken Search repository.

## Local model/runtime surfaces

- Speech-to-text uses `whisper.cpp`
- Whisper model downloads are sourced from Hugging Face URLs declared in `src/AIEngine.cpp`
- Similar-sound category matching optionally uses the pinned LAION CLAP HTSAT unfused audio encoder through ONNX Runtime on the CPU. Audio analysis and inference remain on this device; the application does not upload audio or download this model automatically. Release builders explicitly provision the verified resources described in `docs/similarity-model-build.md`.
- Perceptual matching works without CLAP. Category matching reports an unavailable model rather than silently substituting another algorithm. The deep option compares more candidates and their time sequences locally. Match points are ranking scores, not probabilities.

## Cloud / third-party AI surfaces

- Cover generation is currently offline. The product no longer sends prompts or credentials to a third-party image-generation API.

## Data that may leave the device

- Requests needed to download local Whisper model binaries

## Release expectations

- Do not ship frontend bundles with embedded API keys.
- Require explicit operator action before any cloud AI request is sent.
- If secure secret storage, consent UX, or legal/privacy review is incomplete, disable or exclude the cloud AI surface from the public release.

## Legal review status

- 待法务确认：正式披露文本、外部服务责任边界、地区限制和最终发布条件。
