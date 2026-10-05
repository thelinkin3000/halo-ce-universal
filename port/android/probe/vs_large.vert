// From vs051_0.glsl, dumped by the GL ES build 2d2a322e+changes on the Lenovo TB321FU (debug.gpu_dump_shaders).
// Converted for Vulkan GLSL by a script (tools of the phase, run once) and checked by eye: #version 450;
// the loose uniforms in one std140 block (set 0, binding 0);
// a location on every input and output.
// The bodies are as dumped. Added for the probe: PROBE_SALT (see pass.vert).

#version 450
layout(std140, set = 0, binding = 0) uniform vertex_block
{
	vec4 c[192];
	vec4 viewport_scale;
	vec4 viewport_offset;
	float point_size;
	float screen_offset;
};
layout(location = 0) out vec4 xD0;
layout(location = 1) out vec4 xD1;
layout(location = 2) out vec4 xB0;
layout(location = 3) out vec4 xB1;
layout(location = 4) out vec4 xT0;
layout(location = 5) out vec4 xT1;
layout(location = 6) out vec4 xT2;
layout(location = 7) out vec4 xT3;
layout(location = 8) out float xFog;
layout(constant_id = 0) const int PROBE_SALT = 0;
invariant gl_Position;
vec4 unpack_normpacked3(uint p)
{
	int x = int(p << 21) >> 21;
	int y = int(p << 10) >> 21;
	int z = int(p) >> 22;
	return vec4(float(x) / 1023.0, float(y) / 1023.0, float(z) / 511.0, 1.0);
}
vec4 nv2a_rcc(float x)
{
	float r = 1.0 / x;
	if (r > 0.0) r = clamp(r, 5.42101e-20, 1.884467e+19);
	else r = clamp(r, -1.884467e+19, -5.42101e-20);
	return vec4(r);
}
vec4 nv2a_exp(float x)
{
	return vec4(exp2(floor(x)), fract(x), exp2(x), 1.0);
}
vec4 nv2a_log(float x)
{
	x = abs(x);
	if (x == 0.0) return vec4(-1.0e30, 1.0, -1.0e30, 1.0);
	float e = floor(log2(x));
	return vec4(e, x / exp2(e), log2(x), 1.0);
}
vec4 nv2a_lit(vec4 s)
{
	float specular = s.x > 0.0 ? pow(max(s.y, 0.0), clamp(s.w, -127.9961, 127.9961)) : 0.0;
	return vec4(1.0, max(s.x, 0.0), specular, 1.0);
}
layout(location = 0) in vec4 v0_in;
layout(location = 1) in uint v1_packed;
layout(location = 2) in uint v2_packed;
layout(location = 3) in uint v3_packed;
layout(location = 4) in vec4 v4_in;
layout(location = 5) in vec4 v5_in;
layout(location = 6) in vec4 v6_in;
layout(location = 7) in vec4 v7_in;
layout(location = 8) in vec4 v8_in;
layout(location = 9) in vec4 v9_in;
layout(location = 10) in vec4 v10_in;
layout(location = 11) in vec4 v11_in;
layout(location = 12) in vec4 v12_in;
layout(location = 13) in vec4 v13_in;
layout(location = 14) in vec4 v14_in;
layout(location = 15) in vec4 v15_in;
void main()
{
	vec4 v0 = v0_in;
	vec4 v1 = unpack_normpacked3(v1_packed);
	vec4 v2 = unpack_normpacked3(v2_packed);
	vec4 v3 = unpack_normpacked3(v3_packed);
	vec4 v4 = v4_in;
	vec4 v5 = v5_in;
	vec4 v6 = v6_in;
	vec4 v7 = v7_in;
	vec4 v8 = v8_in;
	vec4 v9 = v9_in;
	vec4 v10 = v10_in;
	vec4 v11 = v11_in;
	vec4 v12 = v12_in;
	vec4 v13 = v13_in;
	vec4 v14 = v14_in;
	vec4 v15 = v15_in;
	vec4 r0 = vec4(0.0), r1 = vec4(0.0), r2 = vec4(0.0), r3 = vec4(0.0);
	vec4 r4 = vec4(0.0), r5 = vec4(0.0), r6 = vec4(0.0), r7 = vec4(0.0);
	vec4 r8 = vec4(0.0), r9 = vec4(0.0), r10 = vec4(0.0), r11 = vec4(0.0);
	vec4 oPos = vec4(0.0, 0.0, 0.0, 1.0);
	vec4 oD0 = vec4(0.0, 0.0, 0.0, 1.0), oD1 = vec4(0.0, 0.0, 0.0, 1.0);
	vec4 oB0 = vec4(0.0, 0.0, 0.0, 1.0), oB1 = vec4(0.0, 0.0, 0.0, 1.0);
	vec4 oT0 = vec4(0.0, 0.0, 0.0, 1.0), oT1 = vec4(0.0, 0.0, 0.0, 1.0);
	vec4 oT2 = vec4(0.0, 0.0, 0.0, 1.0), oT3 = vec4(0.0, 0.0, 0.0, 1.0);
	vec4 oFog = vec4(1.0), oPts = vec4(point_size), oUnused = vec4(0.0);
	int a0 = 0;
	vec4 A, B, C, mac, ilu;
	vec4 clip_position = vec4(0.0);
	bool clip_captured = false;
	/* 0 */
	A = v5.xyzw;
	B = c[7].wwww;
	C = v5.xyzw;
	mac = A * B;
	r2.xy = mac.xy;
	/* 1 */
	A = r2.xxxx;
	B = v0.xyzw;
	C = v0.xyzw;
	mac = A;
	a0 = int(floor(mac.x + 0.001));
	/* 2 */
	A = v6.xxxx;
	B = c[clamp(a0 + 60, 0, 191)].xyzw;
	C = v6.xyzw;
	mac = A * B;
	r3.xyzw = mac.xyzw;
	/* 3 */
	A = v6.xxxx;
	B = c[clamp(a0 + 61, 0, 191)].xyzw;
	C = v6.xyzw;
	mac = A * B;
	r4.xyzw = mac.xyzw;
	/* 4 */
	A = v6.xxxx;
	B = c[clamp(a0 + 62, 0, 191)].xyzw;
	C = v6.xyzw;
	mac = A * B;
	r5.xyzw = mac.xyzw;
	/* 5 */
	A = r2.yyyy;
	B = v0.xyzw;
	C = v0.xyzw;
	mac = A;
	a0 = int(floor(mac.x + 0.001));
	/* 6 */
	A = v6.wwww;
	B = v6.xyzw;
	C = -v6.xxxx;
	mac = A + C;
	r6.x = mac.x;
	/* 7 */
	A = v4.xyzw;
	B = c[12].xyyy;
	C = v4.xyzw;
	mac = A * B;
	oT0.xy = mac.xy;
	/* 8 */
	A = r6.xxxx;
	B = c[clamp(a0 + 60, 0, 191)].xyzw;
	C = r3.xyzw;
	mac = A * B + C;
	r7.xyzw = mac.xyzw;
	/* 9 */
	A = r6.xxxx;
	B = c[clamp(a0 + 61, 0, 191)].xyzw;
	C = r4.xyzw;
	mac = A * B + C;
	r8.xyzw = mac.xyzw;
	/* 10 */
	A = r6.xxxx;
	B = c[clamp(a0 + 62, 0, 191)].xyzw;
	C = r5.xyzw;
	mac = A * B + C;
	r9.xyzw = mac.xyzw;
	/* 11 */
	A = v0.xyzw;
	B = r7.xyzw;
	C = v0.xyzw;
	mac = vec4(dot(A.xyz, B.xyz) + B.w);
	r10.x = mac.x;
	/* 12 */
	A = v0.xyzw;
	B = r8.xyzw;
	C = v0.xyzw;
	mac = vec4(dot(A.xyz, B.xyz) + B.w);
	r10.y = mac.y;
	/* 13 */
	A = v0.xyzw;
	B = r9.xyzw;
	C = v0.xyzw;
	mac = vec4(dot(A.xyz, B.xyz) + B.w);
	r10.z = mac.z;
	/* 14 */
	A = v1.xyzw;
	B = r7.xyzw;
	C = v1.xyzw;
	mac = vec4(dot(A.xyz, B.xyz));
	r11.x = mac.x;
	/* 15 */
	A = v1.xyzw;
	B = r8.xyzw;
	C = v1.xyzw;
	mac = vec4(dot(A.xyz, B.xyz));
	r11.y = mac.y;
	/* 16 */
	A = v1.xyzw;
	B = r9.xyzw;
	C = v1.xyzw;
	mac = vec4(dot(A.xyz, B.xyz));
	r11.z = mac.z;
	/* 17 */
	A = c[17].xyzw;
	B = v0.xyzw;
	C = -r10.xyzw;
	mac = A + C;
	r3.xyz = mac.xyz;
	/* 18 */
	A = r11.xyzw;
	B = r11.xyzw;
	C = v0.xyzw;
	mac = vec4(dot(A.xyz, B.xyz));
	r0.w = mac.w;
	/* 19 */
	A = c[27].xyzw;
	B = v0.xyzw;
	C = v0.xyzw;
	mac = A;
	r8.xyz = mac.xyz;
	/* 20 */
	A = r3.xyzw;
	B = r3.xyzw;
	C = r0.wwww;
	mac = vec4(dot(A.xyz, B.xyz));
	ilu = vec4(inversesqrt(abs(C.x)));
	r4.w = mac.w;
	r1.w = ilu.w;
	/* 21 */
	A = r10.xyzw;
	B = c[0].xyzw;
	C = v0.xyzw;
	mac = vec4(dot(A.xyz, B.xyz) + B.w);
	oPos.x = mac.x;
	/* 22 */
	A = r11.xyzw;
	B = r1.wwww;
	C = v0.xyzw;
	mac = A * B;
	r2.xyz = mac.xyz;
	/* 23 */
	A = r2.xyzw;
	B = -c[23].xyzw;
	C = r4.wwww;
	mac = vec4(dot(A.xyz, B.xyz));
	ilu = vec4(inversesqrt(abs(C.x)));
	r6.z = mac.z;
	r1.w = ilu.w;
	/* 24 */
	A = r4.wwww;
	B = -c[17].wwww;
	C = v4.wwww;
	mac = A * B + C;
	r6.x = mac.x;
	/* 25 */
	A = r3.xyzw;
	B = r1.wwww;
	C = v0.xyzw;
	mac = A * B;
	r5.xyz = mac.xyz;
	/* 26 */
	A = r5.xyzw;
	B = r2.xyzw;
	C = v0.xyzw;
	mac = vec4(dot(A.xyz, B.xyz));
	r7.x = mac.x;
	/* 27 */
	A = r5.xyzw;
	B = c[18].xyzw;
	C = v0.xyzw;
	mac = vec4(dot(A.xyz, B.xyz));
	r8.w = mac.w;
	/* 28 */
	A = c[20].xyzw;
	B = v0.xyzw;
	C = -r10.xyzw;
	mac = A + C;
	r11.xyz = mac.xyz;
	/* 29 */
	A = r8.wwww;
	B = c[18].wwww;
	C = v0.xyzw;
	mac = A * B;
	r9.w = mac.w;
	/* 30 */
	A = r9.wwww;
	B = v0.xyzw;
	C = c[19].wwww;
	mac = A + C;
	r7.z = mac.z;
	/* 31 */
	A = r11.xyzw;
	B = r11.xyzw;
	C = v0.xyzw;
	mac = vec4(dot(A.xyz, B.xyz));
	r11.w = mac.w;
	/* 32 */
	A = r10.xyzw;
	B = c[1].xyzw;
	C = v0.xyzw;
	mac = vec4(dot(A.xyz, B.xyz) + B.w);
	oPos.y = mac.y;
	/* 33 */
	A = r2.xyzw;
	B = -c[25].xyzw;
	C = r11.wwww;
	mac = vec4(dot(A.xyz, B.xyz));
	ilu = vec4(inversesqrt(abs(C.x)));
	r6.w = mac.w;
	r1.w = ilu.w;
	/* 34 */
	A = r11.wwww;
	B = -c[20].wwww;
	C = v4.wwww;
	mac = A * B + C;
	r6.y = mac.y;
	/* 35 */
	A = r11.xyzw;
	B = r1.wwww;
	C = v0.xyzw;
	mac = A * B;
	r0.xyz = mac.xyz;
	/* 36 */
	A = r0.xyzw;
	B = r2.xyzw;
	C = v0.xyzw;
	mac = vec4(dot(A.xyz, B.xyz));
	r7.y = mac.y;
	/* 37 */
	A = r0.xyzw;
	B = c[18].xyzw;
	C = v0.xyzw;
	mac = vec4(dot(A.xyz, B.xyz));
	r2.w = mac.w;
	/* 38 */
	A = r6.xyzw;
	B = v4.zzzz;
	C = v4.xyzw;
	mac = max(A, B);
	r4.xyzw = mac.xyzw;
	/* 39 */
	A = r2.wwww;
	B = c[21].wwww;
	C = v0.xyzw;
	mac = A * B;
	r3.w = mac.w;
	/* 40 */
	A = r3.wwww;
	B = v0.xyzw;
	C = c[22].wwww;
	mac = A + C;
	r7.w = mac.w;
	/* 41 */
	A = r4.xyzw;
	B = v4.wwww;
	C = v4.xyzw;
	mac = min(A, B);
	r6.xyzw = mac.xyzw;
	/* 42 */
	A = r7.xyzw;
	B = v4.zzzz;
	C = v4.xyzw;
	mac = max(A, B);
	r5.xyzw = mac.xyzw;
	/* 43 */
	A = r5.xyzw;
	B = v4.wwww;
	C = v4.xyzw;
	mac = min(A, B);
	r7.xyzw = mac.xyzw;
	/* 44 */
	A = r6.xyzw;
	B = r7.xyzw;
	C = v0.xyzw;
	mac = A * B;
	r9.xy = mac.xy;
	/* 45 */
	A = r9.xyzw;
	B = r7.zwww;
	C = v0.xyzw;
	mac = A * B;
	r11.xy = mac.xy;
	/* 46 */
	A = r11.xxxx;
	B = c[19].xyzw;
	C = r8.xyzw;
	mac = A * B + C;
	r0.xyz = mac.xyz;
	/* 47 */
	A = r11.yyyy;
	B = c[22].xyzw;
	C = r0.xyzw;
	mac = A * B + C;
	r2.xyz = mac.xyz;
	/* 48 */
	A = r6.zzzz;
	B = c[24].xyzw;
	C = r2.xyzw;
	mac = A * B + C;
	r3.xyz = mac.xyz;
	/* 49 */
	A = r6.wwww;
	B = c[26].xyzw;
	C = r3.xyzw;
	mac = A * B + C;
	oD0.xyz = mac.xyz;
	/* 50 */
	A = r10.xyzw;
	B = c[2].xyzw;
	C = v0.xyzw;
	mac = vec4(dot(A.xyz, B.xyz) + B.w);
	oPos.z = mac.z;
	/* 51 */
	A = r10.xyzw;
	B = c[3].xyzw;
	C = v0.xyzw;
	mac = vec4(dot(A.xyz, B.xyz) + B.w);
	oPos.w = mac.w;
	/* 52 */
	A = v4.xyzw;
	B = c[12].zwww;
	C = v4.xyzw;
	mac = A * B;
	oT1.xy = mac.xy;
	/* 53 */
	A = r10.xyzw;
	B = c[9].xyzw;
	C = v0.xyzw;
	mac = vec4(dot(A.xyz, B.xyz) + B.w);
	r4.x = mac.x;
	/* 54 */
	A = r10.xyzw;
	B = c[10].xyzw;
	C = v0.xyzw;
	mac = vec4(dot(A.xyz, B.xyz) + B.w);
	r4.y = mac.y;
	/* 55 */
	A = r10.xyzw;
	B = c[8].xyzw;
	C = v0.xyzw;
	mac = vec4(dot(A.xyz, B.xyz) + B.w);
	r5.z = mac.z;
	/* 56 */
	A = v4.wwww;
	B = v4.xyzw;
	C = -r4.xyzw;
	mac = A + C;
	r5.xy = mac.xy;
	/* 57 */
	A = oPos.xyzw;
	B = c[58].xyzw;
	C = oPos.wwww;
	mac = A * B;
	ilu = nv2a_rcc(C.x);
	clip_position = oPos;
	clip_captured = true;
	r1.x = ilu.x;
	oPos.xyz = mac.xyz;
	/* 58 */
	A = r5.xyzw;
	B = v4.zzzz;
	C = v4.xyzw;
	mac = max(A, B);
	r6.xyz = mac.xyz;
	/* 59 */
	A = r6.xyzw;
	B = r6.xyzw;
	C = v0.xyzw;
	mac = A * B;
	r6.xy = mac.xy;
	/* 60 */
	A = r6.xyzw;
	B = v4.wwww;
	C = v4.xyzw;
	mac = min(A, B);
	r7.xyz = mac.xyz;
	/* 61 */
	A = r7.xxxx;
	B = v0.xyzw;
	C = r7.yyyy;
	mac = A + C;
	r8.x = mac.x;
	/* 62 */
	A = r7.zzzz;
	B = c[11].xxxx;
	C = v0.xyzw;
	mac = A * B;
	r2.z = mac.z;
	/* 63 */
	A = r8.xyzw;
	B = v4.wwww;
	C = v4.xyzw;
	mac = min(A, B);
	r7.x = mac.x;
	/* 64 */
	A = v4.wwww;
	B = v4.xyzw;
	C = -r7.xyzw;
	mac = A + C;
	r9.xy = mac.xy;
	/* 65 */
	A = oPos.xyzw;
	B = r1.xxxx;
	C = c[59].xyzw;
	mac = A * B + C;
	oPos.xyz = mac.xyz;
	/* 66 */
	A = r9.xyzw;
	B = r9.xyzw;
	C = v0.xyzw;
	mac = A * B;
	r10.xy = mac.xy;
	/* 67 */
	A = r10.yyyy;
	B = v0.xyzw;
	C = -r10.xxxx;
	mac = A + C;
	r11.y = mac.y;
	/* 68 */
	A = c[11].yyyy;
	B = r11.yyyy;
	C = r10.xxxx;
	mac = A * B + C;
	r0.w = mac.w;
	/* 69 */
	A = r0.wwww;
	B = c[11].zzzz;
	C = v0.xyzw;
	mac = A * B;
	r2.w = mac.w;
	/* 70 */
	A = -r2.xyzw;
	B = v4.xyzw;
	C = v4.wwww;
	mac = A + C;
	r3.zw = mac.zw;
	/* 71 */
	A = r3.zzzz;
	B = r3.wwww;
	C = v0.xyzw;
	mac = A * B;
	oD0.w = mac.w;
	/* undo the screen-space conversion done with c[-38] and c[-37] */
	vec3 scale = vec3(viewport_scale.x != 0.0 ? viewport_scale.x : 1.0,
		viewport_scale.y != 0.0 ? viewport_scale.y : 1.0,
		viewport_scale.z != 0.0 ? viewport_scale.z : 1.0);
	if (clip_captured)
		gl_Position = vec4((clip_position.xyz * c[58].xyz + (c[59].xyz + vec3(0.5 + screen_offset, 0.5, 0.0)
			- viewport_offset.xyz) * clip_position.w) / scale, clip_position.w);
	else
		gl_Position = vec4((vec3(oPos.xy + vec2(0.5 + screen_offset, 0.5), oPos.z) - viewport_offset.xyz) / scale * oPos.w, oPos.w);
	gl_Position.y = -gl_Position.y;
	gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;
	gl_PointSize = oPts.x;
	xD0 = clamp(oD0, 0.0, 1.0);
	xD1 = clamp(oD1, 0.0, 1.0);
	xB0 = clamp(oB0, 0.0, 1.0);
	xB1 = clamp(oB1, 0.0, 1.0);
	xT0 = oT0;
	xT1 = oT1;
	xT2 = oT2;
	xT3 = oT3;
	xFog = oFog.x;
	gl_Position.x += float(PROBE_SALT) * 1e-30;
}
