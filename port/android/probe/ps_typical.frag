// From ps_e4a3b3ae.glsl, dumped by the GL ES build 2d2a322e+changes on the Lenovo TB321FU (debug.gpu_dump_shaders).
// Converted for Vulkan GLSL by a script (tools of the phase, run once) and checked by eye: #version 450;
// the loose uniforms in one std140 block (set 0, binding 1);
// each sampler a binding of set 0 (2 and up); a location on every input and output.
// The bodies are as dumped. Added for the probe: PROBE_SALT (see pass.vert).

#version 450
layout(std140, set = 0, binding = 1) uniform pixel_block
{
	vec4 ps_c0[8];
	vec4 ps_c1[8];
	vec4 ps_final_c0;
	vec4 ps_final_c1;
	vec4 fog_color;
	vec4 fog_parameters;
	float alpha_reference;
	vec4 bump_matrix[4];
	vec4 bump_luminance[4];
	vec4 texture_scale[4];
	vec4 texture_lod_bias;
};
layout(set = 0, binding = 2) uniform samplerCube tex0;
layout(set = 0, binding = 3) uniform sampler2D tex1;
layout(set = 0, binding = 4) uniform sampler2D tex2;
layout(set = 0, binding = 5) uniform sampler2D tex3;
layout(location = 0) in vec4 xD0;
layout(location = 1) in vec4 xD1;
layout(location = 2) in vec4 xB0;
layout(location = 3) in vec4 xB1;
layout(location = 4) in vec4 xT0;
layout(location = 5) in vec4 xT1;
layout(location = 6) in vec4 xT2;
layout(location = 7) in vec4 xT3;
layout(location = 8) in float xFog;
layout(constant_id = 0) const int PROBE_SALT = 0;
layout(location = 0) out vec4 fragment_color;
float signed_byte(float x)
{
	float b = floor(x * 255.0 + 0.5);
	return (b >= 128.0 ? b - 256.0 : b) / 127.0;
}
vec3 signed_bytes(vec3 x)
{
	return vec3(signed_byte(x.r), signed_byte(x.g), signed_byte(x.b));
}
void main()
{
	vec4 v0 = xD0;
	vec4 v1 = xD1;
	vec4 t0 = vec4(0.0), t1 = vec4(0.0), t2 = vec4(0.0), t3 = vec4(0.0);
	float dot0 = 0.0, dot1 = 0.0, dot2 = 0.0, dot3 = 0.0;
	/* texture stage 0, mode 3 */
	t0 = texture(tex0, (xT0).xyz, texture_lod_bias[0]);
	/* texture stage 1, mode 1 */
	t1 = texture(tex1, (vec4(xT1.xyz / (xT1.w != 0.0 ? xT1.w : 1.0), 1.0)).xy * texture_scale[1].xy, texture_lod_bias[1]);
	/* texture stage 2, mode 0 */
	t2 = vec4(0.0);
	/* texture stage 3, mode 0 */
	t3 = vec4(0.0);
	float fog_factor = xFog;
	vec4 fog = vec4(fog_color.rgb, clamp(fog_factor, 0.0, 1.0));
	vec4 r0 = vec4(0.0, 0.0, 0.0, t0.a);
	vec4 r1 = vec4(0.0);
	/* combiner stage 0 */
	{
		vec3 cA = max(t0.rgb, 0.0);
		vec3 cB = max(vec3(ps_c1[0].a), 0.0);
		vec3 cC = max(t1.rgb, 0.0);
		vec3 cD = max(vec3(ps_c0[0].a), 0.0);
		vec3 cAB = cA * cB;
		vec3 cCD = cC * cD;
		vec3 cSUM = cAB + cCD;
		cAB = clamp((cAB), -1.0, 1.0);
		cCD = clamp((cCD), -1.0, 1.0);
		cSUM = clamp((cSUM), -1.0, 1.0);
		float aA = max(vec4(0.0).b, 0.0);
		float aB = max(vec4(0.0).b, 0.0);
		float aC = max(vec4(0.0).b, 0.0);
		float aD = max(vec4(0.0).b, 0.0);
		float aAB = aA * aB;
		float aCD = aC * aD;
		float aSUM = aAB + aCD;
		aAB = clamp((aAB), -1.0, 1.0);
		aCD = clamp((aCD), -1.0, 1.0);
		aSUM = clamp((aSUM), -1.0, 1.0);
		r0.rgb = cSUM;
	}
	/* combiner stage 1 */
	{
		vec3 cA = max(r0.rgb, 0.0);
		vec3 cB = max(vec3(v0.a), 0.0);
		vec3 cC = max(vec4(0.0).rgb, 0.0);
		vec3 cD = max(vec4(0.0).rgb, 0.0);
		vec3 cAB = cA * cB;
		vec3 cCD = cC * cD;
		vec3 cSUM = cAB + cCD;
		cAB = clamp((cAB), -1.0, 1.0);
		cCD = clamp((cCD), -1.0, 1.0);
		cSUM = clamp((cSUM), -1.0, 1.0);
		float aA = max(vec4(0.0).b, 0.0);
		float aB = max(vec4(0.0).b, 0.0);
		float aC = max(vec4(0.0).b, 0.0);
		float aD = max(vec4(0.0).b, 0.0);
		float aAB = aA * aB;
		float aCD = aC * aD;
		float aSUM = aAB + aCD;
		aAB = clamp((aAB), -1.0, 1.0);
		aCD = clamp((aCD), -1.0, 1.0);
		aSUM = clamp((aSUM), -1.0, 1.0);
		r0.rgb = cSUM;
	}
	vec4 ef_product = vec4(clamp(vec4(0.0).rgb, 0.0, 1.0) * clamp(vec4(0.0).rgb, 0.0, 1.0), 0.0);
	vec4 v1r0_sum = vec4(clamp(v1.rgb, 0.0, 1.0) + clamp(r0.rgb, 0.0, 1.0), 0.0);
	vec3 fA = clamp(vec4(0.0).rgb, 0.0, 1.0);
	vec3 fB = clamp(vec4(0.0).rgb, 0.0, 1.0);
	vec3 fC = clamp(vec4(0.0).rgb, 0.0, 1.0);
	vec3 fD = clamp(r0.rgb, 0.0, 1.0);
	float fG = clamp(r0.a, 0.0, 1.0);
	vec4 result = vec4(fA * fB + (1.0 - fA) * fC + fD, fG);
	fragment_color = clamp(result, 0.0, 1.0);
	fragment_color.a += float(PROBE_SALT) * 1e-30;
}
