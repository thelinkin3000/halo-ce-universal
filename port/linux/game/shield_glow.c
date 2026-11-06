/* Optional shield illumination. Surface appearance and host policy remain
   owned by their existing modules; these lights exist only during rendering.
   From GulchCE, without its Shield Look: the shader is the unit's own, and
   nothing recharges apart from the shader's intensity. */
#include "cseries.h"
#include "real_math.h"
#include "objects/objects.h"
#include "objects/object_definitions.h"
#include "objects/object_lights.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_model_types.h"
#include "shaders/shader_definitions.h"
#include "shaders/shaders.h"
#include "units/units.h"
#include "camera/observer.h"
#include "game/players.h"
#include "network_shield_effects.h"
#include "shield_glow.h"
#include "overshield_effect.h"
#include "plasma_occlusion.h"
#include <math.h>
#include <string.h>

int config_boolean(const char *name);
const char *config_string(const char *name);
unsigned long config_changes(void);
real render_interpolation_game_time_sec(long ticks);
long light_port_glow_definition_get(void);
boolean light_port_glow_set(long light, long owner, real_rgb_color const *color, real radius);
boolean light_port_glow_move(long light, real_point3d const *position);

enum { SHIELD_GLOW_MAXIMUM = 8 };
#define SHIELD_GLOW_RANGE 25.f

/* The validated plasma layout after struct shader, as used by its renderer. */
struct shield_glow_plasma
{
	byte reserved00[4];
	short intensity_exponent_source, pad06;
	real intensity_exponent;
	byte reserved0C[0x2C];
	real perpendicular_alpha;
	real_rgb_color perpendicular_color;
	real parallel_alpha;
	real_rgb_color parallel_color;
	short color_source;
};
struct shield_glow
{
	long unit_index, light_index, rasterizer;
	real strength, last_time, radius;
	boolean connected;
};
struct shield_glow_sample
{
	long unit_index;
	real strength, radius, score;
	real_rgb_color color;
	real_point3d position;
};
static struct shield_glow shield_glows[SHIELD_GLOW_MAXIMUM];
static real shield_glow_scale = 1.f;
static real shield_glow_time;

void shield_glow_reset(void)
{
	short i;
	memset(shield_glows, 0, sizeof(shield_glows));
	for (i = 0; i < SHIELD_GLOW_MAXIMUM; i++)
		shield_glows[i].unit_index = shield_glows[i].light_index = shield_glows[i].rasterizer = NONE;
	shield_glow_time = 0.f;
	overshield_effect_reset();
}

boolean shield_glow_enabled(
	void)
{
	static unsigned long changes = (unsigned long)-1;
	static boolean enabled = FALSE;
	short effects = network_shield_effects();

	if (effects != NONE)
	{
		changes = (unsigned long)-1;
		shield_glow_scale = effects == _shield_effects_light_show ||
			effects == _shield_effects_light_show_overshield ? 3.f : 1.f;
		return effects != _shield_effects_stock && effects != _shield_effects_overshield;
	}
	if (changes != config_changes())
	{
		char const *intensity = config_string("display.shield_glow_intensity");

		changes = config_changes();
		enabled = config_boolean("display.shield_glow") != 0;
		shield_glow_scale = intensity && !strcmp(intensity, "light_show") ? 3.f : 1.f;
	}
	return enabled;
}



static void shield_glow_release(struct shield_glow *glow)
{
	if (glow->light_index != NONE &&
		light_port_glow_set(glow->light_index, NONE, global_real_rgb_black, 0.f))
		light_delete(glow->light_index);
	memset(glow, 0, sizeof(*glow));
	glow->unit_index = glow->light_index = glow->rasterizer = NONE;
}

static struct shield_glow *shield_glow_find(long unit_index)
{
	short i;
	for (i = 0; i < SHIELD_GLOW_MAXIMUM; i++)
		if (shield_glows[i].unit_index == unit_index)
			return &shield_glows[i];
	return NULL;
}

static real shield_glow_camera_distance(real_point3d const *point)
{
	real nearest = SHIELD_GLOW_RANGE * 2.f;
	short player;
	for (player = local_player_get_next(NONE); player != NONE; player = local_player_get_next(player))
	{
		struct observer_result const *camera = observer_get_camera(player);
		if (camera)
			nearest = MIN(nearest, distance3d(point, &camera->position));
	}
	return nearest;
}

static boolean shield_glow_sample(long index, struct shield_glow_sample *sample)
{
	struct object_datum *object = object_try_and_get(index);
	struct object_definition *definition;
	struct shield_glow_plasma const *plasma;
	struct shader *shader;
	real_rgb_color tint = *global_real_rgb_white;
	real_matrix4x3 *nodes;
	real coverage = 1.f, intensity = 1.f, peak, distance, bottom, top;
	short source, i, count;
	boolean overshield;
	if (!object || TEST_FLAG(object->object.flags, _object_invisible_bit) ||
		TEST_FLAG(object->object.damage_flags, _object_dead_bit))
		return FALSE;
	distance = shield_glow_camera_distance(&object->object.bounding_sphere_center);
	if (distance >= SHIELD_GLOW_RANGE)
		return FALSE;
	definition = object_definition_get(object->definition_index);
	if (!definition || definition->object.modifier_shader.index == NONE)
		return FALSE;
	shader = shader_definition_get(definition->object.modifier_shader.index);
	if (!shader || shader->base.type != 10)
		return FALSE;
	plasma = (struct shield_glow_plasma const *)((byte const *)shader + sizeof(struct shader));
	sample->color = plasma->perpendicular_color;
	overshield = overshield_effect_light_sample(index, &tint, &coverage);
	if (overshield)
	{
		/* Match the surface's layer hue while retaining the selected brightness. */
		peak = MAX(sample->color.red, MAX(sample->color.green, sample->color.blue)) /
			MAX(tint.red, MAX(tint.green, tint.blue));
		sample->color.red = tint.red * peak;
		sample->color.green = tint.green * peak;
		sample->color.blue = tint.blue * peak;
		intensity = coverage;
	}
	else
	{
		source = plasma->color_source;
		if (source >= 1 && source <= 4)
		{
			tint = object->object.outgoing_change_colors[source - 1];
			sample->color.red *= tint.red;
			sample->color.green *= tint.green;
			sample->color.blue *= tint.blue;
		}
		source = plasma->intensity_exponent_source;
		if (source >= 1 && source <= 4)
			intensity = (real)pow(MAX(0.f, object->object.outgoing_function_values[source - 1]), plasma->intensity_exponent);
	}
	peak = MAX(sample->color.red, MAX(sample->color.green, sample->color.blue));
	if (!(peak > 0.f) || !valid_real(peak) || !valid_real(intensity))
		return FALSE;
	/* Normalize only overbright shaders; a deliberately dark shader stays dark. */
	peak = MAX(peak, 1.f);
	sample->color.red /= peak; sample->color.green /= peak; sample->color.blue /= peak;
	sample->strength = PIN(intensity * shield_glow_scale, 0.f, 1.f) *
		(1.f - PIN(unit_get(index)->unit.active_camouflage, 0.f, 1.f));
	/* Fade admission over the last three world units, not at a hard distance. */
	sample->strength *= PIN((SHIELD_GLOW_RANGE - distance) / 3.f, 0.f, 1.f);
	count = object->object.node_matrices.size / sizeof(real_matrix4x3);
	if (count <= 0)
		return FALSE;
	nodes = object_get_node_matrices(index);
	bottom = top = nodes[0].position.z;
	for (i = 1; i < count; i++)
	{
		bottom = MIN(bottom, nodes[i].position.z);
		top = MAX(top, nodes[i].position.z);
	}
	/* A radial approximation of the currently luminous body, not an AR cone.
	   Crouching naturally lowers its centre and reduces its reach. */
	sample->position = nodes[0].position;
	sample->position.z = bottom + (top - bottom) * (overshield ? 0.5f * coverage : 0.5f);
	sample->radius = 1.4f * PIN((top - bottom) / 0.9f, 0.65f, 2.f) * shield_glow_scale;
	sample->unit_index = index;
	sample->score = sample->strength * sample->radius * sample->radius / (1.f + distance * distance);
	return TRUE;
}

/* Sorted, bounded selection with an incumbent bonus prevents equal lights
   trading places every view. All local cameras contribute to distance. */
void shield_glow_update(void)
{
	struct shield_glow_sample candidates[SHIELD_GLOW_MAXIMUM], sample;
	struct object_iterator iterator;
	real now = render_interpolation_game_time_sec(game_time_get());
	short count = 0, i, j;
	boolean enabled = shield_glow_enabled();
	if (!enabled || now < shield_glow_time)
		for (i = 0; i < SHIELD_GLOW_MAXIMUM; i++)
			shield_glow_release(&shield_glows[i]);
	shield_glow_time = now;
	if (!enabled)
		return;
	object_iterator_new(&iterator, _object_mask_unit, 0);
	while (object_iterator_next(&iterator))
	{
		struct shield_glow *glow;
		if (!shield_glow_sample(iterator.index, &sample))
			continue;
		glow = shield_glow_find(iterator.index);
		if (glow)
		{
			/* Retain a fading light until its release is visually negligible. */
			sample.score = MAX(sample.score, glow->strength * 0.001f) * 1.2f;
			if (sample.strength < 0.002f && glow->strength < 0.002f)
				continue;
		}
		else if (sample.strength < 0.006f)
			continue;
		for (i = 0; i < count && candidates[i].score >= sample.score; i++) {}
		if (i == SHIELD_GLOW_MAXIMUM)
			continue;
		if (count < SHIELD_GLOW_MAXIMUM) count++;
		for (j = count - 1; j > i; j--) candidates[j] = candidates[j - 1];
		candidates[i] = sample;
	}
	for (i = 0; i < SHIELD_GLOW_MAXIMUM; i++)
	{
		for (j = 0; j < count && candidates[j].unit_index != shield_glows[i].unit_index; j++) {}
		if (j == count) shield_glow_release(&shield_glows[i]);
	}
	for (i = 0; i < count; i++)
	{
		struct shield_glow *glow = shield_glow_find(candidates[i].unit_index);
		real_rgb_color color;
		real blend;
		long definition;
		sample = candidates[i];
		if (!glow)
		{
			for (j = 0; j < SHIELD_GLOW_MAXIMUM && shield_glows[j].unit_index != NONE; j++) {}
			if (j == SHIELD_GLOW_MAXIMUM) continue;
			glow = &shield_glows[j];
			glow->unit_index = sample.unit_index;
			glow->last_time = now;
			glow->strength = sample.strength; /* do not lose a short hit on admission */
		}
		blend = 1.f - (real)exp(-PIN(now - glow->last_time, 0.f, 0.25f) /
			(sample.strength > glow->strength ? 0.015 : 0.08));
		glow->strength += (sample.strength - glow->strength) * blend;
		glow->last_time = now;
		glow->radius = sample.radius;
		color.red = sample.color.red * glow->strength;
		color.green = sample.color.green * glow->strength;
		color.blue = sample.color.blue * glow->strength;
		if (glow->light_index == NONE)
		{
			definition = light_port_glow_definition_get();
			if (definition != NONE)
				glow->light_index = light_new_unattached(definition, NONE, 0, &sample.position, global_forward3d, 0.f);
		}
		if (!light_port_glow_set(glow->light_index, NONE, &color, sample.radius) ||
			!light_port_glow_move(glow->light_index, &sample.position))
			shield_glow_release(glow);
		else
			glow->connected = TRUE;
	}
}

void shield_glow_frame_end(void)
{
	short i;
	boolean enabled = shield_glow_enabled();
	for (i = 0; i < SHIELD_GLOW_MAXIMUM; i++)
	{
		struct shield_glow *glow = &shield_glows[i];
		if (!enabled) shield_glow_release(glow);
		else if (glow->connected && light_port_glow_set(glow->light_index, NONE, global_real_rgb_black, 0.f))
			light_disconnect_from_map(glow->light_index);
		glow->connected = FALSE;
		glow->rasterizer = NONE;
	}
}

void shield_glow_window_begin(void)
{
	short i;
	for (i = 0; i < SHIELD_GLOW_MAXIMUM; i++) shield_glows[i].rasterizer = NONE;
}

void shield_glow_submitted(long light, long rasterizer)
{
	short i;
	for (i = 0; i < SHIELD_GLOW_MAXIMUM; i++)
		if (shield_glows[i].unit_index != NONE && shield_glows[i].light_index == light)
		{
			shield_glows[i].rasterizer = rasterizer;
			plasma_occlusion_submit(light, rasterizer);
		}
}

boolean shield_glow_selected(long rasterizer)
{
	short i;
	if (rasterizer == NONE || !shield_glow_enabled()) return FALSE;
	for (i = 0; i < SHIELD_GLOW_MAXIMUM; i++)
		if (shield_glows[i].unit_index != NONE && shield_glows[i].rasterizer == rasterizer) return TRUE;
	return FALSE;
}

float shield_glow_radius(long light, float radius)
{
	short i;
	for (i = 0; i < SHIELD_GLOW_MAXIMUM; i++)
		if (shield_glows[i].unit_index != NONE && shield_glows[i].light_index == light && light != NONE)
			return MAX(radius, shield_glows[i].radius);
	return radius;
}
