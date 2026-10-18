# Live Voice Firmware

ESPHome firmware for full-duplex conversations through
[LiveVoiceGateway](https://github.com/SandorDrift23/LiveVoiceGateway). The
device performs local wake-word detection and streams PCM audio to the gateway;
OpenAI credentials and Home Assistant access stay on the gateway.

The microphone remains open while the assistant speaks. A spoken interruption
or the center button immediately drops buffered playback. The same custom
`live_voice` ESPHome component is shared by the Voice PE and experimental
ReSpeaker configurations.

> [!NOTE]
> This project was developed with substantial generative-AI assistance
> ("KI"), using Cursor and AI models for implementation, review, and
> documentation. The maintainer directed the work and validates it on real
> hardware. AI assistance does not replace compilation or hardware testing.

## Hardware status

- **Home Assistant Voice PE:** current development and tested target.
- **ReSpeaker XVF3800:** not built and not tested on ReSpeaker hardware. Its
  configuration is an experimental starting point, not a ready-to-flash image.

## Home Assistant Voice PE

`boards/voice-pe.yaml` uses the Voice PE audio path: XMOS `ffva_v1.3.1`, the
AIC3204 codec, 16 kHz microphone capture, and 24 kHz gateway playback into the
ESPHome resampler.

Current behavior:

- Alexa or Okay Nabu wake word and the center button start a conversation.
- The center button also stops an active conversation.
- The mute switch blocks conversations and turns the ring orange.
- The ring is red while the gateway is unavailable, pulses while a
  conversation is active, and is off while idle.
- The dial changes speaker volume in five-percent steps.
- The microphone continues streaming during assistant playback.

Acoustic echo cancellation has not been independently verified, so the device
reports `aec_verified: false`. Start at low volume and test through the 3.5 mm
output before relying on full-duplex barge-in with the internal speaker.

### Install

1. Install and configure
   [LiveVoiceGateway](https://github.com/SandorDrift23/LiveVoiceGateway).
2. Copy `boards/voice-pe.example.yaml` into the ESPHome configuration
   directory and adapt its substitutions and secrets.
3. Make this repository's `esphome/components` directory available to ESPHome
   at the path used by `external_components`.
4. Set `va_url` to `ws://<gateway-ip>:8080/`.
5. Validate and compile the YAML in ESPHome before flashing.

The firmware depends on ESPHome, the official Voice PE `voice_kit` component,
the official XMOS firmware image, and ESPHome wake-word models. Those upstream
resources are referenced by URL and are not maintained in this repository.

## ReSpeaker XVF3800

`boards/respeaker-xvf3800.yaml` describes a proposed Seeed XVF3800 path with
the XVF3800 as 48 kHz I²S master, microphone data on GPIO43, and speaker data
on GPIO44. The gateway can resample its declared 48 kHz capture rate to the
24 kHz GPT-Live rate.

This configuration has **not been built and has not been tested on a
ReSpeaker**. It does not yet load the XVF `inthost` firmware, configure acoustic
echo cancellation, or drive the AIC3104 codec. ESPHome may also reject the
shared clock pins used by the separate receive and transmit controllers.

## Protocol

The device starts with a compact JSON hello:

```json
{"type":"hello","protocol":1,"device":"voice_pe","input_rate":16000,"output_rate":24000,"duplex":true,"aec_verified":false}
```

Binary frames are little-endian mono PCM16. The device sends `wake` and
`interrupt` control messages. The gateway sends `phase` and `flush` messages.

## Upstream software

This project builds on ESPHome and references:

- [ESPHome](https://github.com/esphome/esphome)
- [Home Assistant Voice PE](https://github.com/esphome/home-assistant-voice-pe)
- [Voice Kit XMOS firmware](https://github.com/esphome/voice-kit-xmos-firmware)

The local `aic3204` component originated in ESPHome and contains a small
compatibility adjustment for component discovery. ESPHome's Python and
configuration code is MIT licensed; its C/C++ runtime code is GPL-3.0 licensed.
Keep the applicable upstream license and attribution when redistributing it.
