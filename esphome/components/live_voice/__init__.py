import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import esp32, microphone, speaker
from esphome.const import CONF_ID, CONF_MICROPHONE, CONF_SPEAKER, CONF_TRIGGER_ID, CONF_URL

CODEOWNERS = ["@livevoice"]
DEPENDENCIES = ["network", "microphone", "speaker"]

CONF_DEVICE = "device"
CONF_MIC_CHANNEL = "mic_channel"
CONF_INPUT_RATE = "input_rate"
CONF_OUTPUT_RATE = "output_rate"
CONF_AEC_VERIFIED = "aec_verified"
CONF_ON_PHASE = "on_phase"

live_voice_ns = cg.esphome_ns.namespace("live_voice")
LiveVoice = live_voice_ns.class_("LiveVoice", cg.Component)
OnPhaseTrigger = live_voice_ns.class_("OnPhaseTrigger", automation.Trigger.template(cg.std_string))

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(LiveVoice),
        cv.Required(CONF_URL): cv.string,
        cv.Required(CONF_DEVICE): cv.string,
        cv.Required(CONF_MICROPHONE): cv.use_id(microphone.Microphone),
        cv.Required(CONF_SPEAKER): cv.use_id(speaker.Speaker),
        cv.Optional(CONF_MIC_CHANNEL, default=0): cv.int_range(min=0, max=1),
        cv.Required(CONF_INPUT_RATE): cv.int_range(min=8000, max=48000),
        cv.Required(CONF_OUTPUT_RATE): cv.int_range(min=8000, max=48000),
        cv.Optional(CONF_AEC_VERIFIED, default=False): cv.boolean,
        cv.Optional(CONF_ON_PHASE): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OnPhaseTrigger)}
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    esp32.add_idf_component(name="espressif/esp_websocket_client", ref="1.7.0")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_url(config[CONF_URL]))
    cg.add(var.set_device(config[CONF_DEVICE]))
    cg.add(var.set_microphone(await cg.get_variable(config[CONF_MICROPHONE])))
    cg.add(var.set_speaker(await cg.get_variable(config[CONF_SPEAKER])))
    cg.add(var.set_mic_channel(config[CONF_MIC_CHANNEL]))
    cg.add(var.set_input_rate(config[CONF_INPUT_RATE]))
    cg.add(var.set_output_rate(config[CONF_OUTPUT_RATE]))
    cg.add(var.set_aec_verified(config[CONF_AEC_VERIFIED]))
    for conf in config.get(CONF_ON_PHASE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "phase")], conf)
