/*
HOST_VK_DRIVER.H

Opens the Vulkan driver the renderer runs on (port/android/VULKAN.md, "The
driver module"): the phone's own, or one the player left in the data folder
as an adrenotools archive, loaded with libadrenotools. Used by the probe and,
later, by the backend.
*/

#ifndef HOST_VK_DRIVER_H
#define HOST_VK_DRIVER_H

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <stddef.h>

/* Opens the driver named by setting (display.vk_driver): empty is the phone's
own; otherwise the name of an archive in the app's external files folder. It
returns that driver's vkGetInstanceProcAddr, or NULL when not even the phone's
driver could be opened (after logging why). A driver archive that cannot be
used is logged and the phone's own is opened instead. description says which
driver was opened and, if it fell back, why, in one line, for the log and the
probe's report.

Only one driver is open in the process: a second call returns the first call's
result. Nothing in the host may load the phone's Vulkan driver once a custom
one is open (no SDL_Vulkan_* calls: SDL would dlopen the system loader). Not
thread safe; call it from the thread that sets Vulkan up. */
PFN_vkGetInstanceProcAddr host_vk_driver_open(const char *setting, char *description, size_t size);

/* After the instance is made (the system loader loads a driver only then): whether the driver asked for is the one
loaded, in text. libadrenotools hands back the system loader whatever it is given, and its hook falls back to the phone's
own driver without a word when the library will not load, so a custom driver's library must be looked for in the
process: returns 0 and says so if it is not there. True for the phone's own driver. */
int host_vk_driver_verify(char *text, size_t size);

/* closes the driver; every Vulkan object made through it must be destroyed
first, and nothing may call into it afterwards */
void host_vk_driver_close(void);

#endif
