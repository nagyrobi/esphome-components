from esphome import automation
import esphome.codegen as cg
from esphome.components import display
import esphome.config_validation as cv
from esphome.const import (
    CONF_DIMENSIONS,
    CONF_HEIGHT,
    CONF_ID,
    CONF_LAMBDA,
    CONF_ON_CONNECT,
    CONF_ON_DISCONNECT,
    CONF_PORT,
    CONF_TRIGGER_ID,
    CONF_WIDTH,
    PLATFORM_ESP32,
    PLATFORM_HOST,
)
from esphome.core import Lambda

from . import VNCDisplay, VNCTrigger

DEPENDENCIES = ["network"]
AUTO_LOAD = ["socket", "touchscreen"]

# The transmit task needs FreeRTOS or pthreads.
SUPPORTED_PLATFORMS = [PLATFORM_ESP32, PLATFORM_HOST]

# FULL_DISPLAY_SCHEMA already extends cv.polling_component_schema("1s") and makes
# `lambda` and `pages` mutually exclusive, so neither needs repeating here.
CONFIG_SCHEMA = cv.All(
    display.FULL_DISPLAY_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(VNCDisplay),
            cv.Optional(CONF_PORT, default=5900): cv.port,
            cv.Required(CONF_DIMENSIONS): cv.Any(
                cv.dimensions,
                cv.Schema(
                    {
                        cv.Required(CONF_WIDTH): cv.int_range(min=1, max=32767),
                        cv.Required(CONF_HEIGHT): cv.int_range(min=1, max=32767),
                    }
                ),
            ),
            cv.Optional(CONF_ON_CONNECT): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(VNCTrigger),
                }
            ),
            cv.Optional(CONF_ON_DISCONNECT): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(VNCTrigger),
                }
            ),
        }
    ),
    cv.only_on(SUPPORTED_PLATFORMS),
)


async def _setup_trigger(var, config, key, setter):
    if (conf := config.get(key)) is None:
        return
    conf = conf[0]
    trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
    await automation.build_automation(trigger, [], conf)
    lamb = await cg.process_lambda(
        Lambda(f"{trigger}->trigger();"), [], return_type=cg.void
    )
    cg.add(setter(lamb))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await display.register_display(var, config)
    cg.add(var.set_port(config[CONF_PORT]))

    if lambconf := config.get(CONF_LAMBDA):
        lambda_ = await cg.process_lambda(
            lambconf, [(display.DisplayRef, "it")], return_type=cg.void
        )
        cg.add(var.set_writer(lambda_))

    await _setup_trigger(var, config, CONF_ON_CONNECT, var.set_on_connect)
    await _setup_trigger(var, config, CONF_ON_DISCONNECT, var.set_on_disconnect)

    dimensions = config[CONF_DIMENSIONS]
    if isinstance(dimensions, dict):
        cg.add(var.set_dimensions(dimensions[CONF_WIDTH], dimensions[CONF_HEIGHT]))
    else:
        (width, height) = dimensions
        cg.add(var.set_dimensions(width, height))
