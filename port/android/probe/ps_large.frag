// From ps_297f687f.glsl, dumped by the GL ES build 2d2a322e+changes on the Lenovo TB321FU (debug.gpu_dump_shaders).
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
layout(set = 0, binding = 2) uniform sampler2D tex0;
layout(set = 0, binding = 3) uniform sampler2D tex1;
layout(set = 0, binding = 4) uniform sampler2D tex2;
layout(set = 0, binding = 5) uniform samplerCube tex3;
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
	/* texture stage 0, mode 1 */
	t0 = texture(tex0, (vec4(xT0.xyz / (xT0.w != 0.0 ? xT0.w : 1.0), 1.0)).xy * texture_scale[0].xy, texture_lod_bias[0]);
	/* texture stage 1, mode 1 */
	t1 = texture(tex1, (vec4(xT1.xyz / (xT1.w != 0.0 ? xT1.w : 1.0), 1.0)).xy * texture_scale[1].xy, texture_lod_bias[1]);
	/* texture stage 2, mode 1 */
	t2 = texture(tex2, (vec4(xT2.xyz / (xT2.w != 0.0 ? xT2.w : 1.0), 1.0)).xy * texture_scale[2].xy, texture_lod_bias[2]);
	/* texture stage 3, mode 3 */
	t3 = texture(tex3, (xT3).xyz, texture_lod_bias[3]);
	float fog_factor = xFog;
	vec4 fog = vec4(fog_color.rgb, clamp(fog_factor, 0.0, 1.0));
	vec4 r0 = vec4(0.0, 0.0, 0.0, t0.a);
	vec4 r1 = vec4(0.0);
	/* combiner stage 0 */
	{
		vec3 cA = max(t2.rgb, 0.0);
		vec3 cB = max(ps_c1[0].rgb, 0.0);
		vec3 cC = max(t2.rgb, 0.0);
		vec3 cD = max(ps_c0[0].rgb, 0.0);
		vec3 cAB = vec3(dot(cA, cB));
		vec3 cCD = vec3(dot(cC, cD));
		vec3 cSUM = cAB + cCD;
		cAB = clamp((cAB), -1.0, 1.0);
		cCD = clamp((cCD), -1.0, 1.0);
		cSUM = clamp((cSUM), -1.0, 1.0);
		float aA = max(t2.b, 0.0);
		float aB = (1.0 - clamp(vec4(0.0).b, 0.0, 1.0));
		float aC = max(vec4(0.0).b, 0.0);
		float aD = max(vec4(0.0).b, 0.0);
		float aAB = aA * aB;
		float aCD = aC * aD;
		float aSUM = aAB + aCD;
		aAB = clamp((aAB), -1.0, 1.0);
		aCD = clamp((aCD), -1.0, 1.0);
		aSUM = clamp((aSUM), -1.0, 1.0);
		r0.rgb = cAB;
		r1.rgb = cCD;
		r0.a = aAB;
	}
	/* combiner stage 1 */
	{
		vec3 cA = max(v0.rgb, 0.0);
		vec3 cB = (1.0 - clamp(vec4(0.0).rgb, 0.0, 1.0));
		vec3 cC = max(r0.rgb, 0.0);
		vec3 cD = max(ps_c0[1].rgb, 0.0);
		vec3 cAB = cA * cB;
		vec3 cCD = cC * cD;
		vec3 cSUM = cAB + cCD;
		cAB = clamp((cAB), -1.0, 1.0);
		cCD = clamp((cCD), -1.0, 1.0);
		cSUM = clamp((cSUM), -1.0, 1.0);
		float aA = max(r0.a, 0.0);
		float aB = (1.0 - clamp(vec4(0.0).b, 0.0, 1.0));
		float aC = max(vec4(0.0).b, 0.0);
		float aD = max(vec4(0.0).b, 0.0);
		float aAB = aA * aB;
		float aCD = aC * aD;
		float aSUM = aAB + aCD;
		aAB = clamp((aAB), -1.0, 1.0);
		aCD = clamp((aCD), -1.0, 1.0);
		aSUM = clamp((aSUM), -1.0, 1.0);
		v0.rgb = cSUM;
		t1.a = aAB;
	}
	/* combiner stage 2 */
	{
		vec3 cA = (1.0 - clamp(vec3(r0.a), 0.0, 1.0));
		vec3 cB = (1.0 - clamp(vec4(0.0).rgb, 0.0, 1.0));
		vec3 cC = max(vec3(r0.a), 0.0);
		vec3 cD = max(ps_c0[2].rgb, 0.0);
		vec3 cAB = cA * cB;
		vec3 cCD = cC * cD;
		vec3 cSUM = cAB + cCD;
		cAB = clamp((cAB), -1.0, 1.0);
		cCD = clamp((cCD), -1.0, 1.0);
		cSUM = clamp((cSUM), -1.0, 1.0);
		float aA = max(r0.b, 0.0);
		float aB = (1.0 - clamp(vec4(0.0).b, 0.0, 1.0));
		float aC = max(r1.b, 0.0);
		float aD = max(v1.a, 0.0);
		float aAB = aA * aB;
		float aCD = aC * aD;
		float aSUM = aAB + aCD;
		aAB = clamp((aAB), -1.0, 1.0);
		aCD = clamp((aCD), -1.0, 1.0);
		aSUM = clamp((aSUM), -1.0, 1.0);
		r0.rgb = cSUM;
		r0.a = aAB;
		r1.a = aCD;
	}
	/* combiner stage 3 */
	{
		vec3 cA = max(vec4(0.0).rgb, 0.0);
		vec3 cB = (0.5 - max(vec4(0.0).rgb, 0.0));
		vec3 cC = (1.0 - clamp(vec4(0.0).rgb, 0.0, 1.0));
		vec3 cD = max(t1.rgb, 0.0);
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
		t1.rgb = cSUM;
	}
	/* combiner stage 4 */
	{
		vec3 cA = max(t3.rgb, 0.0);
		vec3 cB = max(v1.rgb, 0.0);
		vec3 cC = max(v0.rgb, 0.0);
		vec3 cD = max(r0.rgb, 0.0);
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
		t3.rgb = cAB;
		v0.rgb = cCD;
	}
	/* combiner stage 5 */
	{
		vec3 cA = max(ps_c1[5].rgb, 0.0);
		vec3 cB = (1.0 - clamp(vec4(0.0).rgb, 0.0, 1.0));
		vec3 cC = max(vec3(v0.a), 0.0);
		vec3 cD = (-ps_c0[5].rgb);
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
		r1.rgb = cSUM;
	}
	/* combiner stage 6 */
	{
		vec3 cA = max(t0.rgb, 0.0);
		vec3 cB = max(t1.rgb, 0.0);
		vec3 cC = max(t0.rgb, 0.0);
		vec3 cD = max(t1.rgb, 0.0);
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
		t0.rgb = cSUM;
	}
	/* combiner stage 7 */
	{
		vec3 cA = max(t0.rgb, 0.0);
		vec3 cB = max(v0.rgb, 0.0);
		vec3 cC = max(t3.rgb, 0.0);
		vec3 cD = max(vec3(r1.a), 0.0);
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
	vec4 ef_product = vec4(clamp(r0.rgb, 0.0, 1.0) * clamp(vec3(ps_final_c0.a), 0.0, 1.0), 0.0);
	vec4 v1r0_sum = vec4(clamp(v1.rgb, 0.0, 1.0) + clamp(r0.rgb, 0.0, 1.0), 0.0);
	vec3 fA = (1.0 - clamp(vec3(v0.a), 0.0, 1.0));
	vec3 fB = clamp(ef_product.rgb, 0.0, 1.0);
	vec3 fC = clamp(ps_final_c0.rgb, 0.0, 1.0);
	vec3 fD = clamp(r1.rgb, 0.0, 1.0);
	float fG = clamp(t0.a, 0.0, 1.0);
	vec4 result = vec4(fA * fB + (1.0 - fA) * fC + fD, fG);
	if (!(floor(clamp(result.a, 0.0, 1.0) * 255.0 + 0.5) > alpha_reference)) discard;
	fragment_color = clamp(result, 0.0, 1.0);
	fragment_color.a += float(PROBE_SALT) * 1e-30;
}
