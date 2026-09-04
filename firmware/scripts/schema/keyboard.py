# This program is free software: you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
# details.
#
# You should have received a copy of the GNU General Public License along with
# this program. If not, see <https://www.gnu.org/licenses/>.

from enum import Enum
from pydantic import BaseModel, Field, NonNegativeInt, PositiveFloat, PositiveInt


class KeyboardUSBPort(str, Enum):
    FULL_SPEED = "fs"
    HIGH_SPEED = "hs"


# USB Configuration
class KeyboardUSB(BaseModel):
    # USB Vendor ID
    vid: str = Field(pattern=r"^0x[0-9A-Fa-f]{4}$")
    # USB Product ID
    pid: str = Field(pattern=r"^0x[0-9A-Fa-f]{4}$")
    port: KeyboardUSBPort


# Keyboard Configuration
class KeyboardKeyboard(BaseModel):
    num_profiles: int = Field(ge=1, le=8)
    num_layers: int = Field(ge=1, le=8)
    num_keys: int = Field(ge=1, le=256)
    num_advanced_keys: int = Field(ge=1, le=64)


# Hardware Configuration
class KeyboardHardware(BaseModel):
    # High-speed external oscillator value in Hz
    hse_value: PositiveInt
    # Keyboard driver name
    driver: str


# Raw ADC input configuration
class KeyboardAnalogRaw(BaseModel):
    # Array of raw ADC input channels. If a string is provided, it is used as the GPIO pin name
    input: list[NonNegativeInt | str]
    # Array of key index mappings for the raw ADC inputs
    vector: list[NonNegativeInt]


# Analog Multiplexer ADC input configuration
class KeyboardAnalogMux(BaseModel):
    # Array of GPIO pin names for the multiplexer select lines
    select: list[str]
    # Array of ADC input channels for the multiplexers. If a string is provided, it is used as the GPIO pin name
    input: list[NonNegativeInt | str]
    # Mapping from multiplexers to ADC input channels
    matrix: list[list[NonNegativeInt]]


# Analog Configuration
class KeyboardAnalog(BaseModel):
    # ADC resolution for this keyboard. A higher value means higher accuracy but slower matrix scans. Default to the maximum resolution supported by the MCU if not provided.
    adc_resolution: PositiveInt | None = None
    # Set to true if the ADC value is inversely proportional to the travel distance of the keys
    invert_adc: bool = False
    # List of 1-based key indices that need individual ADC inversion
    invert_adc_keys: list[PositiveInt] | None = None
    # List of 1-based key indices forced to a fixed ON/OFF actuation. These keys
    # bypass the configured actuation point and Rapid Trigger and instead use a
    # raw-ADC Schmitt trigger (see fixed_onoff_on_delta / fixed_onoff_off_delta).
    # Intended as a hardware workaround for keys whose magnet-sensor gap produces
    # too little ADC travel for normal analog actuation. The web configurator
    # still shows AP/RP for these keys, but they have no effect on actuation.
    fixed_onoff_keys: list[PositiveInt] | None = None
    # Raw-ADC deviation from the resting value at which a fixed ON/OFF key turns
    # ON, and the (lower) deviation at which it turns OFF (hysteresis band).
    fixed_onoff_on_delta: PositiveInt = 10
    fixed_onoff_off_delta: NonNegativeInt = 5
    # Delay in microseconds between ADC scans
    delay: int | None = None
    raw: KeyboardAnalogRaw | None = None
    mux: KeyboardAnalogMux | None = None


# Calibration Configuration
class KeyboardCalibration(BaseModel):
    # See `include/lib/eeconfig.h`
    initial_rest_value: NonNegativeInt
    # See `include/lib/eeconfig.h`
    initial_bottom_out_threshold: NonNegativeInt
    # Bottom-out dead zone in travel-distance counts (0-255). See
    # `eeconfig_calibration_t` in `include/eeconfig.h`. Keep the default in sync
    # with `DEFAULT_BOTTOM_OUT_DEADZONE`.
    bottom_out_deadzone: NonNegativeInt = 10


# Wear leveling Configuration
class KeyboardWearLeveling(BaseModel):
    # Size of the virtual persistent storage in bytes. There must be enough RAM of this size to hold the entire virtual storage.
    virtual_size: int = Field(ge=1, le=8192)
    # Size of the write log in bytes
    write_log_size: int = Field(ge=1, le=65536)


class KeyboardLayoutKey(BaseModel):
    key: NonNegativeInt
    w: PositiveFloat | None = None
    h: PositiveFloat | None = None
    x: float | None = None
    y: float | None = None
    # Option key and value pairs for the corresponding labels
    option: tuple[int, int] | None = None


# Keyboard Layout
class KeyboardLayout(BaseModel):
    # Labels for each layout option. Use a string for a toggle option, or an array for a select option
    labels: list[str | list[str]] | None = None
    # Metadata for how the keyboard should be rendered in the web configurator
    keymap: list[list[KeyboardLayoutKey]]


# Actuation Configuration
class KeyboardActuation(BaseModel):
    # Default actuation point
    actuation_point: int = Field(ge=0, le=255)


# OLED display layout
class KeyboardOLEDLayout(str, Enum):
    # 3x3 key grid with an ADC column below it (Archetype09)
    GRID3X3 = "grid3x3"
    # Sensor tester: key grid taken from `layout.keymap`, a detail view of the
    # last pressed key, overview pages while idle, optional NTC temperature
    TESTER = "tester"


# NTC thermistor read through a spare analog key as a voltage divider
class KeyboardOLEDNTC(BaseModel):
    # 1-based key index whose ADC reading is the divider midpoint
    key: PositiveInt
    # Beta constant of the thermistor (K)
    beta: PositiveFloat = 3380
    # Thermistor resistance at 25 C (ohm)
    r25: PositiveFloat = 10000
    # Fixed resistor of the divider (ohm)
    r_series: PositiveFloat = 10000
    # True when the thermistor sits between the supply and the midpoint with
    # the fixed resistor to ground; false when the thermistor is on the ground
    # side
    ntc_to_vcc: bool = True


# OLED Configuration
class KeyboardOLED(BaseModel):
    # Enable OLED display support
    enabled: bool = False
    layout: KeyboardOLEDLayout = KeyboardOLEDLayout.GRID3X3
    # Short label per key, 1 to 3 characters, indexed by 0-based key (tester)
    key_labels: list[str] | None = None
    ntc: KeyboardOLEDNTC | None = None
    # Tester: milliseconds without a key press before the overview pages cycle
    idle_timeout_ms: PositiveInt = 5000
    # Tester: milliseconds each overview page stays on screen
    page_interval_ms: PositiveInt = 3000


# keyboard.json Schema
class Keyboard(BaseModel):
    name: str
    manufacturer: str
    maintainer: str
    usb: KeyboardUSB
    keyboard: KeyboardKeyboard
    hardware: KeyboardHardware
    analog: KeyboardAnalog
    calibration: KeyboardCalibration
    wear_leveling: KeyboardWearLeveling | None = None
    oled: KeyboardOLED | None = None
    layout: KeyboardLayout
    # Default keymap
    keymap: list[list[str]] | None = None
    # Default keymaps for each profile. If not specified, the default keymap will be used for all profiles.
    keymaps: list[list[list[str]]] | None = None
    actuation: KeyboardActuation | None = None
