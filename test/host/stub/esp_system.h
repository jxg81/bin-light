#pragma once
// Host stub: reboot is a no-op in the render/parse harnesses, which never
// take the /wifi-forget path.
static inline void esp_restart(void) {}
