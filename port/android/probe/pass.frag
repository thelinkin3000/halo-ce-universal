// The probe's own pass-through pixel shader (steps 3 and 5). PROBE_SALT is
// explained in pass.vert.
#version 450

layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 out_color;
layout(constant_id = 0) const int PROBE_SALT = 0;

void main()
{
	out_color = v_color;
	out_color.a += float(PROBE_SALT) * 1e-30;
}
