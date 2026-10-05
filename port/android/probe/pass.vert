// The probe's own pass-through vertex shader (steps 3 and 5): a position and a
// colour in, the colour on to the pixel shader.
//
// PROBE_SALT is the specialization constant step 5 gives each run a different
// value of, so that a pipeline made in one run is not one the driver made
// before: it is added where no compiler can drop it for any value it is given.
#version 450

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec4 in_color;
layout(location = 0) out vec4 v_color;
layout(constant_id = 0) const int PROBE_SALT = 0;

void main()
{
	gl_Position = vec4(in_position, 0.0, 1.0);
	v_color = in_color;
	gl_Position.x += float(PROBE_SALT) * 1e-30;
}
