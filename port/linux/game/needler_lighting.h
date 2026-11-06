#ifndef NEEDLER_LIGHTING_H
#define NEEDLER_LIGHTING_H

int needler_lighting_position(long weapon, real_point3d *position, real_vector3d *direction);

struct shader;
void needler_lighting_material(struct shader *shader, real_rgb_color *color);
void needler_lighting_forget(void);
struct pixel_shader_definition;
void needler_lighting_transparent(struct shader *shader, struct pixel_shader_definition *pixel, int stages);

#endif
