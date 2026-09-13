import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32, speaker
from esphome.const import CONF_ID

CODEOWNERS = ["@local"]
DEPENDENCIES = ["esp32", "speaker"]
AUTO_LOAD = ["audio"]

sound_cache_ns = cg.esphome_ns.namespace("sound_cache")
SoundCache = sound_cache_ns.class_("SoundCache", cg.Component)

CONF_SPEAKER = "speaker"
CONF_BASE_URL = "base_url"
CONF_PARTITION_LABEL = "partition_label"
CONF_RESERVE_BYTES = "reserve_bytes"
CONF_MAX_FILE_SIZE = "max_file_size"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SoundCache),
        cv.Required(CONF_SPEAKER): cv.use_id(speaker.Speaker),
        cv.Required(CONF_BASE_URL): cv.url,
        cv.Optional(CONF_PARTITION_LABEL, default="soundcache"): cv.string_strict,
        cv.Optional(CONF_RESERVE_BYTES, default=262144): cv.positive_int,
        cv.Optional(CONF_MAX_FILE_SIZE, default=4194304): cv.positive_int,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    esp32.include_builtin_idf_component("esp_http_client")
    esp32.include_builtin_idf_component("spiffs")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    spk = await cg.get_variable(config[CONF_SPEAKER])
    cg.add(var.set_speaker(spk))
    cg.add(var.set_base_url(config[CONF_BASE_URL]))
    cg.add(var.set_partition_label(config[CONF_PARTITION_LABEL]))
    cg.add(var.set_reserve_bytes(config[CONF_RESERVE_BYTES]))
    cg.add(var.set_max_file_size(config[CONF_MAX_FILE_SIZE]))
