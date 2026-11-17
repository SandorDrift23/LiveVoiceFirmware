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
- **ReSpeaker XVF3800:** configuration includes the XMOS I2S firmware update,
  codec, and 16 kHz capture path. It has not been compiled or tested on
  ReSpeaker hardware.

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

`boards/respeaker-xvf3800.yaml` uses the XVF3800 as 48 kHz I²S master.
Microphone data is on GPIO43 and speaker data is on GPIO44. The DSP output is
band-limited to 8 kHz, so a decimator keeps one frame in three and the gateway
receives 16 kHz. Playback is 24 kHz from the gateway, resampled to 48 kHz.
The image embeds Seeed I2S master firmware **1.0.9** and writes it over I²C
when the chip version differs. The AIC3104 digital volume is set on boot. AEC
stays the firmware default and is reported as unverified.

A factory board still running the USB firmware cannot take that I²C update.
Flash `respeaker_xvf3800_i2s_master_v1.0.9_48k.bin` once with `dfu-util` on the
XMOS USB-C port, then flash this ESPHome image on the XIAO port. Later 1.0.9
updates happen at boot. Do not flash the USB 2.x images; they have no I²S
output and no I²C update path.

The ring follows the Voice PE status colours: red while the gateway is down,
orange while muted, a pulse during a conversation, and off while idle.

This configuration has **not been compiled and has not been tested on a
ReSpeaker**.

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
- [reSpeaker XVF3800 firmware](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY)
- [reSpeaker XVF3800 ESPHome integration](https://github.com/formatBCE/Respeaker-XVF3800-ESPHome-integration)

The local `aic3204` component originated in ESPHome and contains a small
compatibility adjustment for component discovery. ESPHome's Python and
configuration code is MIT licensed; its C/C++ runtime code is GPL-3.0 licensed.
Keep the applicable upstream license and attribution when redistributing it.
