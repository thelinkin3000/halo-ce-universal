/* A fake rendered world exercising the complete shield light implementation. */
#include <math.h>
#include <string.h>
#include <stdio.h>
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-function"
typedef int boolean;
typedef float real;
typedef unsigned char byte;
typedef struct {real red,green,blue;} real_rgb_color;
typedef struct {real x,y,z;} real_point3d;
typedef struct {real i,j,k;} real_vector3d;
typedef struct {real_point3d position;} real_matrix4x3;
#define TRUE 1
#define FALSE 0
#define NONE (-1)
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define PIN(a,b,c) MIN(MAX(a,b),c)
#define TEST_FLAG(a,b) ((a)&(1<<(b)))
#define _object_invisible_bit 0
#define _object_dead_bit 0
#define _object_mask_unit 1
#define CHECK(x) do {if (!(x)) {printf("line %d\n",__LINE__);return 1;}} while(0)
static real_rgb_color black={0,0,0}, white={1,1,1};
static real_rgb_color const *global_real_rgb_black=&black,*global_real_rgb_white=&white;
static real_vector3d forward={1,0,0};
static real_vector3d const *global_forward3d=&forward;
struct object_fields {unsigned flags,damage_flags;real_point3d bounding_sphere_center;real outgoing_function_values[4];real_rgb_color outgoing_change_colors[4];struct{int size;}node_matrices;};
struct object_datum {long definition_index;struct object_fields object;};
struct unit_datum {long definition_index;struct object_fields object;struct{real active_camouflage;}unit;};
static struct unit_datum units[12];
static real_matrix4x3 poses[12][2];
struct object_definition {struct{struct{long index;}modifier_shader;}object;};
static struct object_definition definition;
struct shader {struct{int type;}base;};
struct render_model_effect {struct shader *modifier_shader;struct{real const *values;real_rgb_color const *colors;}modifier_animation;};
struct observer_result {real_point3d position;};
static struct observer_result camera;
struct object_iterator {long index;};
static int unit_count=1,on=1,os;
static short policy=NONE;
static real clock_time,coverage=1;
static unsigned long changes;
static int allocations,deletions,disconnects,submissions,resets;
static int valid_lights[128];
static real_rgb_color last_color[128];
static real_point3d last_position[128];
static real last_radius[128];
static char const *intensity="default";
int config_boolean(char const *name){return on;}
char const *config_string(char const *name){return intensity;}
unsigned long config_changes(void){return changes;}
static short network_shield_effects(void){return policy;}
static void overshield_effect_reset(void){resets++;}
static real render_interpolation_game_time_sec(long t){return clock_time;}
static long game_time_get(void){return (long)(clock_time*30);}
static long light_port_glow_definition_get(void){return 9;}
static boolean light_port_glow_set(long l,long o,real_rgb_color const *c,real r){if(l<0||l>=128||!valid_lights[l])return 0;last_color[l]=*c;last_radius[l]=r;return 1;}
static boolean light_port_glow_move(long l,real_point3d const *p){if(l<0||l>=128||!valid_lights[l])return 0;last_position[l]=*p;return 1;}
static long light_new_unattached(long d,long o,int n,real_point3d const *p,real_vector3d const *f,real t){int l=++allocations;valid_lights[l]=1;return l;}
static void light_delete(long l){valid_lights[l]=0;deletions++;}
static void light_disconnect_from_map(long l){disconnects++;}
static void plasma_occlusion_submit(long l,long r){submissions++;}
static void object_iterator_new(struct object_iterator *i,int mask,int f){i->index=-1;}
static int object_iterator_next(struct object_iterator *i){return ++i->index<unit_count;}
static struct object_datum *object_try_and_get(long i){return i>=0&&i<unit_count?(struct object_datum*)&units[i]:NULL;}
static struct unit_datum *unit_get(long i){return &units[i];}
static struct object_definition *object_definition_get(long i){return &definition;}
static real_matrix4x3 *object_get_node_matrices(long i){return poses[i];}
static short local_player_get_next(short i){return i==NONE?0:NONE;}
static struct observer_result const *observer_get_camera(short i){return &camera;}
static real distance3d(real_point3d const *a,real_point3d const *b){real x=a->x-b->x,y=a->y-b->y,z=a->z-b->z;return (real)sqrt(x*x+y*y+z*z);}
static int valid_real(real x){return isfinite(x);}
static struct shader *shader_definition_get(long i);
static boolean overshield_effect_light_sample(long i,real_rgb_color *c,real *v){if(!os)return 0;*v=coverage;c->red=os==1?.306f:.992f;c->green=os==1?.992f:.306f;c->blue=.455f;return 1;}
#include "shield_under_test.inc"
static struct {struct shader shader;struct shield_glow_plasma plasma;} original, appearance;
static struct shader *shader_definition_get(long i){return &original.shader;}
static int close_to(real a,real b){return fabs(a-b)<.0001;}
int main(void)
{
 int i,rate,step;real result[3];long light;struct shield_glow_sample sample;
 original.shader.base.type=appearance.shader.base.type=10;
 original.plasma.intensity_exponent_source=appearance.plasma.intensity_exponent_source=1;
 original.plasma.intensity_exponent=appearance.plasma.intensity_exponent=1;
 original.plasma.perpendicular_color=white;
 appearance.plasma.perpendicular_color.red=.2f;appearance.plasma.perpendicular_color.green=.4f;appearance.plasma.perpendicular_color.blue=1;
 for(i=0;i<12;i++){units[i].object.node_matrices.size=sizeof(poses[i]);poses[i][1].position.z=.9f;units[i].object.outgoing_function_values[0]=1;units[i].object.outgoing_change_colors[0]=white;units[i].object.bounding_sphere_center.x=20-i;}
 shield_glow_reset();shield_glow_update();CHECK(allocations==1);
 light=shield_glows[0].light_index;CHECK(last_radius[light]>1&&close_to(last_position[light].z,.45f));
 shield_glow_submitted(light,3);CHECK(shield_glow_selected(3)&&submissions==1);
 shield_glow_frame_end();CHECK(disconnects==1&&!shield_glow_selected(3)&&last_radius[light]==0);
 clock_time+=.1f;shield_glow_update();
 os=1;shield_glow_update();CHECK(last_color[light].green>last_color[light].red);
 os=2;shield_glow_update();CHECK(last_color[light].red>last_color[light].green);
 coverage=.2f;CHECK(shield_glow_sample(0,&sample)&&close_to(sample.position.z,.09f));
 os=0;original.plasma.color_source=1;units[0].object.outgoing_change_colors[0].green=.1f;
 CHECK(shield_glow_sample(0,&sample)&&sample.color.green<sample.color.red);
 units[0].unit.active_camouflage=1;CHECK(shield_glow_sample(0,&sample)&&sample.strength==0);units[0].unit.active_camouflage=0;
 units[0].object.damage_flags=1;shield_glow_update();CHECK(deletions==1);units[0].object.damage_flags=0;
 for(rate=0;rate<3;rate++){
  int fps=rate==0?30:rate==1?60:120;
  shield_glow_reset();clock_time=0;units[0].object.outgoing_function_values[0]=1;shield_glow_update();units[0].object.outgoing_function_values[0]=0;
  for(step=1;step<=fps/5;step++){clock_time=(real)step/fps;shield_glow_update();}
  result[rate]=shield_glows[0].strength;
 }
 CHECK(close_to(result[0],result[1])&&close_to(result[1],result[2]));
 for(step=1;step<50;step++){clock_time+=.03f;shield_glow_update();}CHECK(shield_glow_find(0)==NULL);
 unit_count=12;for(i=0;i<12;i++)units[i].object.outgoing_function_values[0]=1;
 shield_glow_update();CHECK(shield_glow_find(11)&&!shield_glow_find(0));
 light=shield_glow_find(11)->light_index;shield_glow_update();CHECK(shield_glow_find(11)->light_index==light);
 on=0;changes++;shield_glow_update();for(i=0;i<8;i++)CHECK(shield_glows[i].unit_index==NONE);
 policy=_shield_effects_glow;CHECK(shield_glow_enabled());shield_glow_update();CHECK(shield_glow_find(11));
 policy=_shield_effects_stock;on=1;changes++;shield_glow_frame_end();CHECK(!shield_glow_enabled()&&!shield_glow_find(11));
 shield_glow_reset();CHECK(resets>0);return 0;
}
