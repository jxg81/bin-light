#pragma once
// Host stand-in for the generated sdkconfig.h. Only the options the modules
// under test actually read. Keep in step with main/Kconfig.projbuild - a
// missing option here is a compile error, not a silent skip.
#define CONFIG_BINLIGHT_RESET_BUTTON_GPIO         2
#define CONFIG_BINLIGHT_RESET_BUTTON_ACTIVE_LOW   1
#define CONFIG_BINLIGHT_ACTION_BUTTON_GPIO        1
// Action button is active-HIGH (touch module), so - exactly as Kconfig does
// for a false bool - CONFIG_BINLIGHT_ACTION_BUTTON_ACTIVE_LOW is left undefined.
