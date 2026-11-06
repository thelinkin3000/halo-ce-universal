/* Game/render-thread lighting preferences. Host policy is checked separately. */
#ifndef LIGHTING_CONFIG_H
#define LIGHTING_CONFIG_H
int lighting_config_boolean(const char *name);
/* debugging: reads a setting as off for this run, without changing it; 0 if the
   setting is not one of these */
int lighting_force_off(const char *name, int forced);
int lighting_forced_off(const char *name);
#endif
