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
    CONF_PASSWORD,
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


def _validate_password(value):
    """RFB VNC Authentication derives a single DES key from the password, so only the
    first eight bytes are significant and clients disagree on non-ASCII encodings."""
    value = cv.string_strict(value)
    if not 1 <= len(value) <= 8:
        raise cv.Invalid(
            f"password must be between 1 and 8 characters (got {len(value)}) - the RFB "
            "authentication scheme derives a DES key from it and ignores the rest"
        )
    if not value.isascii():
        raise cv.Invalid(
            "password must be ASCII - clients do not agree on other encodings"
        )
    return value


def _consume_vnc_sockets(config):
    """Declare this component's socket usage so the platform can size its socket pool.

    Each vnc display holds one listening socket for the lifetime of the component and
    one accepted socket while a client is attached.
    """
    from esphome.components import socket

    socket.consume_sockets(1, "vnc", socket.SocketType.TCP_LISTEN)(config)
    socket.consume_sockets(1, "vnc")(config)
    return config


# FULL_DISPLAY_SCHEMA already extends cv.polling_component_schema("1s") and makes
# `lambda` and `pages` mutually exclusive, so neither needs repeating here.
CONFIG_SCHEMA = cv.All(
    display.FULL_DISPLAY_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(VNCDisplay),
            cv.Optional(CONF_PORT, default=5900): cv.port,
            cv.Optional(CONF_PASSWORD): cv.sensitive(_validate_password),
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
    _consume_vnc_sockets,
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
    if (password := config.get(CONF_PASSWORD)) is not None:
        cg.add(var.set_password(password))

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
