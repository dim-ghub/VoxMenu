# Vox Menu Documentation

Welcome to the Vox Menu documentation. Here you will find comprehensive guides on the architecture, text-to-speech engines, configuration options, and development workflows.

## Documentation Index

- [Architecture & Internals](architecture.md)
  - How Vox Menu runs inside Nightcap without virtual audio cables or companion apps.
  - SDL3 function wrapping, keystroke reconstruction, microphone injection, the Vulkan overlay, the HUD and automation, and the Nightcap host integration (log and packet observers, launch broker).

- [Speech Engines Guide](engines.md)
  - Detailed overview of the three supported local speech engines:
    - **Piper (Neural AI)**: High-quality ONNX neural voices.
    - **eSpeak-NG**: Lightweight multi-accent retro formant speech.
    - **Animal Crossing (Animalese)**: Authentic villager voice synthesizer with English and Korean sound banks.
  - Instructions on downloading and managing custom voices and letter sound packs.

- [Configuration Reference](configuration.md)
  - Every `config.json` setting (TTS, HUD, game, autoclicker, macros, hotkeys) and the macro file format.
  - Audio routing mechanics, volume control, and microphone mixing.
  - Automatic migration from `vox-tts` and `nightcap-ac`.

- [Development & Testing Guide](development.md)
  - Prerequisites and build instructions.
  - Test harness design: how `vk_driver`, `mic_driver`, and Python integration tests simulate Roblox and Nightcap.
  - Environment variables for debugging and headless rendering.
