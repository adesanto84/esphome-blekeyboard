"""BleKeyboard component."""

from __future__ import annotations

from typing import Final

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.automation import maybe_simple_id
from esphome.components import binary_sensor, button, number
from esphome.components.esp32 import add_idf_sdkconfig_option, include_builtin_idf_component
from esphome.const import (
    CONF_BATTERY_LEVEL,
    CONF_CODE,
    CONF_DELAY,
    CONF_ID,
    CONF_INITIAL_VALUE,
    CONF_MANUFACTURER_ID,
    CONF_MAX_VALUE,
    CONF_MIN_VALUE,
    CONF_NAME,
    CONF_STEP,
    CONF_TYPE,
    CONF_VALUE,
)
from esphome.core import CORE, ID
from esphome.cpp_generator import LambdaExpression, MockObj, TemplateArguments

from .const import (
    ACTION_COMBINATION_CLASS,
    ACTION_PRESS_CLASS,
    ACTION_PRINT_CLASS,
    ACTION_RELEASE_CLASS,
    ACTION_START_CLASS,
    ACTION_STOP_CLASS,
    BINARY_SENSOR_STATE,
    BUTTONS_KEY,
    COMPONENT_BUTTON_CLASS,
    COMPONENT_CLASS,
    COMPONENT_NUMBER_CLASS,
    CONF_BUTTONS,
    CONF_KEYS,
    CONF_RECONNECT,
    CONF_TEXT,
    DOMAIN,
    NUMBERS,
)

CODEOWNERS: Final = ["@dmamontov", "@adesanto84"]
AUTO_LOAD: Final = ["binary_sensor", "number", "button"]

ble_keyboard_ns = cg.esphome_ns.namespace(DOMAIN)

BLEKeyboard = ble_keyboard_ns.class_(COMPONENT_CLASS, cg.PollingComponent)
BLEKeyboardNumber = ble_keyboard_ns.class_(COMPONENT_NUMBER_CLASS, cg.Component)
BLEKeyboardButton = ble_keyboard_ns.class_(COMPONENT_BUTTON_CLASS, cg.Component)

CONFIG_SCHEMA: Final = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(BLEKeyboard),
        cv.Optional(CONF_NAME, default=COMPONENT_CLASS): cv.Length(min=1),
        cv.Optional(CONF_MANUFACTURER_ID, default=COMPONENT_CLASS): cv.Length(min=1),
        cv.Optional(CONF_BATTERY_LEVEL, default=100): cv.int_range(min=0, max=100),
        cv.Optional(CONF_RECONNECT, default=True): cv.boolean,
        cv.Optional(CONF_BUTTONS, default=True): cv.boolean,
    }
)


async def to_code(config: dict) -> None:
    """Generate component."""

    if not CORE.is_esp32:
        raise cv.Invalid("The component only supports ESP32.")

    if CORE.using_arduino:
        raise cv.Invalid("The component only supports the ESP-IDF framework.")
    # The esp_hid component (which provides esp_hidd_dev_init,
    # esp_hidd_dev_input_set, esp_hidd_dev_battery_set, etc.) is excluded
    # from the ESPHome ESP-IDF build by default to keep the binary small.
    # We use it for the BLE keyboard implementation, so un-exclude it.
    include_builtin_idf_component("esp_hid")

    # The esp_hid device library dispatches esp_hidd_dev_init to a NimBLE
    # backend (esp_ble_hidd_dev_init, in esp_hid/src/nimble_hidd.c) or a
    # Bluedroid backend (esp_bt_hidd_dev_init, in esp_hid/src/bt_hidd.c).
    # The NimBLE backend is the one we want, but the entire body of
    # esp_ble_hidd_dev_init is wrapped in `#if CONFIG_BT_NIMBLE_HID_SERVICE`,
    # so without that option set the linker fails with:
    #
    #   undefined reference to `esp_ble_hidd_dev_init'
    #
    # We force the option here (in addition to the user's yaml
    # sdkconfig_options) so a missing entry in the yaml does not break
    # the build. The component's Kconfig.projbuild does the same thing
    # for plain PlatformIO + ESP-IDF projects that don't go through
    # ESPHome's `add_idf_sdkconfig_option` path.
    add_idf_sdkconfig_option("CONFIG_BT_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_BLE_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_NIMBLE_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_NIMBLE_HID_SERVICE", True)

    var = cg.new_Pvariable(
        config[CONF_ID],
        config[CONF_NAME],
        config[CONF_MANUFACTURER_ID],
        config[CONF_BATTERY_LEVEL],
        config[CONF_RECONNECT],
    )

    await cg.register_component(var, config)

    await adding_binary_sensors(var)
    await adding_numbers(var)

    if config[CONF_BUTTONS]:
        await adding_buttons(var)


async def adding_buttons(var: MockObj) -> None:
    """Adding buttons."""

    for key in BUTTONS_KEY:
        new_key: MockObj = await button.new_button(
            key | {CONF_ID: cv.declare_id(BLEKeyboardButton)(key[CONF_ID])}
        )
        cg.add(new_key.set_parent(var))

        if CONF_VALUE not in key:
            continue

        if isinstance(key[CONF_VALUE], tuple):
            cg.add(new_key.set_value(*key[CONF_VALUE]))
        else:
            cg.add(new_key.set_value(key[CONF_VALUE]))


async def adding_numbers(var: MockObj) -> None:
    """Adding numbers."""

    for num in NUMBERS:
        number_delay: MockObj = await number.new_number(
            num
            | {
                CONF_ID: cv.declare_id(BLEKeyboardNumber)(num[CONF_ID]),
            },
            min_value=num[CONF_MIN_VALUE],
            max_value=num[CONF_MAX_VALUE],
            step=num[CONF_STEP],
        )
        cg.add(number_delay.set_parent(var))
        cg.add(number_delay.set_initial_value(num[CONF_INITIAL_VALUE]))
        cg.add(number_delay.set_type(num[CONF_TYPE]))

        cg.add(number_delay.setup())


async def adding_binary_sensors(var: MockObj) -> None:
    """Adding binary sensor."""

    cg.add(
        var.set_state_sensor(await binary_sensor.new_binary_sensor(BINARY_SENSOR_STATE))
    )


OPERATION_BASE_SCHEMA: Final = cv.Schema(
    {
        cv.Required(CONF_ID): cv.use_id(BLEKeyboard),
    }
)

BLEKeyboardReleaseAction = ble_keyboard_ns.class_(
    ACTION_RELEASE_CLASS, automation.Action
)


@automation.register_action(
    f"{DOMAIN}.release",
    BLEKeyboardReleaseAction,
    maybe_simple_id(OPERATION_BASE_SCHEMA),
)
async def ble_keyboard_release_to_code(
    config: dict, action_id: ID, template_arg: TemplateArguments, args: list
) -> MockObj:
    """Action release."""

    paren: MockObj = await cg.get_variable(config[CONF_ID])

    return cg.new_Pvariable(action_id, template_arg, paren)


BLEKeyboardPrintAction = ble_keyboard_ns.class_(ACTION_PRINT_CLASS, automation.Action)


@automation.register_action(
    f"{DOMAIN}.print",
    BLEKeyboardPrintAction,
    OPERATION_BASE_SCHEMA.extend(
        {
            cv.Required(CONF_TEXT): cv.templatable(cv.string_strict),
        }
    ),
)
async def ble_keyboard_print_to_code(
    config: dict, action_id: ID, template_arg: TemplateArguments, args: list
) -> MockObj:
    """Action print."""

    paren: MockObj = await cg.get_variable(config[CONF_ID])
    var: MockObj = cg.new_Pvariable(action_id, template_arg, paren)
    template_: LambdaExpression = await cg.templatable(
        config[CONF_TEXT], args, cg.std_string
    )

    cg.add(var.set_text(template_))

    return var


BLEKeyboardPressAction = ble_keyboard_ns.class_(ACTION_PRESS_CLASS, automation.Action)


@automation.register_action(
    f"{DOMAIN}.press",
    BLEKeyboardPressAction,
    OPERATION_BASE_SCHEMA.extend(
        {
            cv.Required(CONF_CODE): cv.Any(
                cv.templatable(cv.positive_int),
                cv.All(
                    [cv.uint8_t],
                    cv.Length(min=2, max=2),
                ),
            ),
        }
    ),
)
async def ble_keyboard_press_to_code(
    config: dict, action_id: ID, template_arg: TemplateArguments, args: list
) -> MockObj:
    """Action press."""

    paren: MockObj = await cg.get_variable(config[CONF_ID])
    var: MockObj = cg.new_Pvariable(action_id, template_arg, paren)

    if isinstance(config[CONF_CODE], list):
        cg.add(var.set_keys(config[CONF_CODE]))
    else:
        template_: LambdaExpression = await cg.templatable(config[CONF_CODE], args, int)

        cg.add(var.set_code(template_))

    return var


BLEKeyboardCombinationAction = ble_keyboard_ns.class_(
    ACTION_COMBINATION_CLASS, automation.Action
)


@automation.register_action(
    f"{DOMAIN}.combination",
    BLEKeyboardCombinationAction,
    OPERATION_BASE_SCHEMA.extend(
        {
            cv.Required(CONF_DELAY): cv.templatable(cv.positive_int),
            cv.Required(CONF_KEYS): cv.All(
                [cv.Any(cv.string_strict, cv.uint8_t)],
                cv.Length(min=2, max=5),
            ),
        }
    ),
)
async def ble_keyboard_combination_to_code(
    config: dict, action_id: ID, template_arg: TemplateArguments, args: list
) -> MockObj:
    """Action combination."""

    paren: MockObj = await cg.get_variable(config[CONF_ID])
    var: MockObj = cg.new_Pvariable(action_id, template_arg, paren)
    template_: LambdaExpression = await cg.templatable(config[CONF_DELAY], args, cg.uint32)

    cg.add(var.set_delay(template_))
    cg.add(var.set_keys([str(key) for key in config[CONF_KEYS]]))

    return var


BLEKeyboardStartAction = ble_keyboard_ns.class_(ACTION_START_CLASS, automation.Action)


@automation.register_action(
    f"{DOMAIN}.start",
    BLEKeyboardStartAction,
    maybe_simple_id(OPERATION_BASE_SCHEMA),
)
async def ble_keyboard_start_to_code(
    config: dict, action_id: ID, template_arg: TemplateArguments, args: list
) -> MockObj:
    """Action start."""

    paren: MockObj = await cg.get_variable(config[CONF_ID])

    return cg.new_Pvariable(action_id, template_arg, paren)


BLEKeyboardStopAction = ble_keyboard_ns.class_(ACTION_STOP_CLASS, automation.Action)


@automation.register_action(
    f"{DOMAIN}.stop",
    BLEKeyboardStopAction,
    maybe_simple_id(OPERATION_BASE_SCHEMA),
)
async def ble_keyboard_stop_to_code(
    config: dict, action_id: ID, template_arg: TemplateArguments, args: list
) -> MockObj:
    """Action stop."""

    paren: MockObj = await cg.get_variable(config[CONF_ID])

    return cg.new_Pvariable(action_id, template_arg, paren)
