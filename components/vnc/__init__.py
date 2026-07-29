from esphome import automation
import esphome.codegen as cg
from esphome.components import display

CODEOWNERS = ["@clydebarrow", "@nagyrobi"]

vnc_ns = cg.esphome_ns.namespace("vnc")
VNCDisplay = vnc_ns.class_(
    "VNCDisplay",
    display.Display,
)
VNCTrigger = vnc_ns.class_(
    "VNCTrigger",
    automation.Trigger.template(),
    cg.Parented.template(VNCDisplay),
)

CONF_VNC_ID = "vnc_id"
