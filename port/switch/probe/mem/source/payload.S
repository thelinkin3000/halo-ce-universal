/*
TEST CODE FOR MEM PROBE 5

The payload the probe executes, assembled rather than hand-encoded. Probe 4
hand-wrote the instruction as bytes and got the immediate wrong: the bytes
a0 1e 8d 52 are "mov w0, #0x68f5", not "mov w0, #0x5a5a". The probe then
concluded the code had not run, when in fact it had run perfectly and
returned the wrong constant. So the payload lives here, where the assembler
gets it right, and main.c copies these bytes into the mapping.

The marker is deliberately an unlikely value: if it comes back intact,
nothing is truncating the return.
*/

.section .text.halo_probe, "ax"

.global halo_probe_payload
.global halo_probe_payload_end

halo_probe_payload:
	mov	w0, #0x5a5a		/* the value the probe expects back */
	ret

halo_probe_payload_end: