#include "harness.h"
static unsigned long generation;
static int enabled, reads, change_at;
unsigned long config_changes(void) { return generation; }
int config_boolean(const char *name)
{ reads++;if(reads==change_at){enabled=1;generation++;}return enabled; }
#include "under_test.inc"
int main(int argc,char **argv)
{
 unsigned int i;int before;
 if(argc>1&&!strcmp(argv[1],"limited"))
 {
  enabled=1;generation++;
  CHECK(!lighting_config_boolean("display.muzzle_lighting"),"a key the platform leaves out reads as off");
  CHECK(lighting_config_boolean("display.plasma_lighting"),"and the others as they are");
  return 0;
 }
 if(argc>1&&!strcmp(argv[1],"forced"))
 {
  enabled=1;generation++;
  CHECK(lighting_config_boolean("display.muzzle_lighting"),"a setting on in the config reads on");
  CHECK(lighting_force_off("display.muzzle_lighting",1),"a lighting setting can be forced off");
  CHECK(lighting_forced_off("display.muzzle_lighting"),"and says so");
  CHECK(!lighting_config_boolean("display.muzzle_lighting"),"forced off reads off with the config on");
  CHECK(lighting_config_boolean("display.plasma_lighting"),"the others are as the config has them");
  generation++;
  CHECK(!lighting_config_boolean("display.muzzle_lighting"),"it outlasts a settings change");
  CHECK(lighting_force_off("display.muzzle_lighting",0),"it can be lifted");
  CHECK(lighting_config_boolean("display.muzzle_lighting"),"lifted, the config decides again, with no settings change");
  enabled=0;generation++;
  CHECK(lighting_force_off("display.muzzle_lighting",0)&&!lighting_config_boolean("display.muzzle_lighting"),"lifting never turns on what the config has off");
  CHECK(!lighting_force_off("unrelated.setting",1),"only lighting settings can be forced");
  CHECK(!lighting_forced_off("unrelated.setting"),"and an unrelated one is not forced");
  return 0;
 }
 CHECK(!lighting_config_boolean("display.plasma_lighting"),"initial zero generation loads off defaults");
 before=reads;
 for(i=0;i<10000;i++) CHECK(!lighting_config_boolean(preferences[i%13].name),"cached value remains off");
 CHECK(reads==before,"ordinary reads do not reenter general configuration");
 enabled=1;generation++;
 for(i=0;i<13;i++) CHECK(lighting_config_boolean(preferences[i].name),"apply refreshes every related preference");
 CHECK(reads==before+13,"one refresh for a settings generation");
 enabled=0;generation++;
 CHECK(!lighting_config_boolean("display.vehicle_lighting"),"defaults/disable is visible immediately");
 before=reads;CHECK(!lighting_config_boolean("unrelated.setting")&&reads==before+1,"unknown names retain ordinary lookup behavior");
 generation++;change_at=reads+2;
 CHECK(!lighting_config_boolean("display.plasma_lighting"),"first value was read before intervening change");
 CHECK(lighting_config_boolean("display.plasma_lighting"),"intervening change cannot leave a stale value cached");
 return 0;
}
