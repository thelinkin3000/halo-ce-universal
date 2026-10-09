/*
HOST_MIC_PROBE.C

A probe for voice chat's microphone (config.toml's debug.mic_probe), before
voice chat is ported: upstream's voice_audio.c reads the microphone through
SDL, and the Switch's SDL2 cannot record, so the host would read it through
libnx's audin. What audin hears in this program is not written down
anywhere: its default input ("BuiltInHeadset", libnx's audin.c says) is
likely a headset in the console's jack, and whether a USB microphone, a
headset through the dock, or a program that has taken over a title's place
(this one) is heard at all is not known.

So for PROBE_SECONDS from the start, on a helper core, this lists the inputs
the console offers (again every PROBE_LIST_EVERY seconds, so a headset
plugged in during the run shows), opens the default one as libnx does, says
what it was given (rate, channels, format), and logs once a second how many
buffers came back and how loud they were: the peak and the RMS in dBFS of
the first channel, so a talking voice, a quiet room and an input that only
delivers zeros can be told apart in halo.log.
*/

#include "host.h"

#include <switch.h>

#include <malloc.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define PROBE_SECONDS 120
#define PROBE_LIST_EVERY 10
/* buffers of 20 ms at 48 kHz, 2 channels of 16 bits, as voice_audio.c's
frames are long; each rounded up to the 0x1000 bytes audin wants */
#define PROBE_FRAME_SAMPLES 960
#define PROBE_BUFFERS 4
#define PROBE_MAXIMUM_DEVICES 8
#define PROBE_DEVICE_NAME_SIZE 0x100

struct probe_level
{
	unsigned long buffers, samples, zero_buffers;
	int peak;
	double square_sum;
};

static void list_inputs(void)
{
	static char names[PROBE_MAXIMUM_DEVICES * PROBE_DEVICE_NAME_SIZE];
	u32 count = 0, index;
	Result result;

	memset(names, 0, sizeof(names));
	result = audinListAudioIns(names, PROBE_MAXIMUM_DEVICES, &count);
	if (R_FAILED(result))
	{
		host_logf(HOST_LOG_ERROR, "mic probe: listing the inputs failed: 0x%x", (unsigned)result);
		return;
	}
	host_logf(HOST_LOG_INFO, "mic probe: %u input(s)", (unsigned)count);
	for (index = 0; index < count && index < PROBE_MAXIMUM_DEVICES; index++)
	{
		char *name = names + index * PROBE_DEVICE_NAME_SIZE;

		name[PROBE_DEVICE_NAME_SIZE - 1] = 0;
		host_logf(HOST_LOG_INFO, "mic probe:   \"%s\"", name);
	}
}

/* the first channel's 16-bit samples of a buffer that came back */
static void measure(struct probe_level *level, const AudioInBuffer *buffer, u32 channels)
{
	const s16 *samples = buffer->buffer;
	unsigned long count = channels ? buffer->data_size / 2 / channels : 0, index;
	int any = 0;

	for (index = 0; index < count; index++)
	{
		int value = samples[index * channels];
		int magnitude = value < 0 ? -value : value;

		if (magnitude > level->peak)
			level->peak = magnitude;
		if (value)
			any = 1;
		level->square_sum += (double)value * value;
	}
	level->buffers++;
	level->samples += count;
	if (!any)
		level->zero_buffers++;
}

static double dbfs(double value)
{
	return value > 0.0 ? 20.0 * log10(value / 32768.0) : -999.0;
}

static void log_level(struct probe_level *level, unsigned seconds)
{
	double rms = level->samples ? sqrt(level->square_sum / (double)level->samples) : 0.0;

	host_logf(HOST_LOG_INFO, "mic probe: %3us: %lu buffers (%lu all zeros), %lu samples, peak %.1f dBFS, "
		"rms %.1f dBFS", seconds, level->buffers, level->zero_buffers, level->samples, dbfs(level->peak), dbfs(rms));
	memset(level, 0, sizeof(*level));
}

static void *probe_thread(void *unused)
{
	AudioInBuffer buffers[PROBE_BUFFERS];
	u8 *data[PROBE_BUFFERS] = {0};
	u32 data_size, buffer_size, channels, index;
	struct probe_level level;
	u64 start, last_log, last_list;
	Result result;

	(void)unused;
	host_thread_place_on_helper_core();
	host_logf(HOST_LOG_INFO, "mic probe: started (debug.mic_probe), for %d s", PROBE_SECONDS);

	/* (audinInitialize also opens the default input, as libnx does) */
	result = audinInitialize();
	if (R_FAILED(result))
	{
		host_logf(HOST_LOG_ERROR, "mic probe: audinInitialize failed: 0x%x (module %u, description %u)",
			(unsigned)result, (unsigned)R_MODULE(result), (unsigned)R_DESCRIPTION(result));
		return NULL;
	}
	list_inputs();
	channels = audinGetChannelCount();
	host_logf(HOST_LOG_INFO, "mic probe: the default input opened: %u Hz, %u channel(s), format %d "
		"(2: 16-bit), state %d (0: started, 1: stopped)", (unsigned)audinGetSampleRate(), (unsigned)channels,
		(int)audinGetPcmFormat(), (int)audinGetDeviceState());
	if (audinGetPcmFormat() != PcmFormat_Int16 || !channels)
	{
		host_logf(HOST_LOG_ERROR, "mic probe: not 16-bit samples; the levels below are not measured");
		channels = 0;
	}

	data_size = PROBE_FRAME_SAMPLES * (channels ? channels : 2) * 2;
	buffer_size = (data_size + 0xfff) & ~0xfffu;
	for (index = 0; index < PROBE_BUFFERS; index++)
	{
		data[index] = memalign(0x1000, buffer_size);
		if (!data[index])
		{
			host_logf(HOST_LOG_ERROR, "mic probe: out of memory for the buffers");
			goto done;
		}
		memset(data[index], 0, buffer_size);
		buffers[index].next = NULL;
		buffers[index].buffer = data[index];
		buffers[index].buffer_size = buffer_size;
		buffers[index].data_size = data_size;
		buffers[index].data_offset = 0;
	}

	result = audinStartAudioIn();
	if (R_FAILED(result))
	{
		host_logf(HOST_LOG_ERROR, "mic probe: audinStartAudioIn failed: 0x%x", (unsigned)result);
		goto done;
	}
	for (index = 0; index < PROBE_BUFFERS; index++)
	{
		result = audinAppendAudioInBuffer(&buffers[index]);
		if (R_FAILED(result))
			host_logf(HOST_LOG_ERROR, "mic probe: audinAppendAudioInBuffer failed: 0x%x", (unsigned)result);
	}

	memset(&level, 0, sizeof(level));
	start = last_log = last_list = armTicksToNs(armGetSystemTick());
	for (;;)
	{
		AudioInBuffer *released = NULL;
		u32 released_count = 0;
		u64 now;

		/* a second at most, so a silent input still logs */
		result = audinWaitCaptureFinish(&released, &released_count, 1000000000ULL);
		while (R_SUCCEEDED(result) && released)
		{
			if (channels)
				measure(&level, released, channels);
			released->data_size = data_size;
			audinAppendAudioInBuffer(released);
			/* the others that came back with it */
			released = NULL;
			released_count = 0;
			result = audinGetReleasedAudioInBuffer(&released, &released_count);
			if (!released_count)
				break;
		}

		now = armTicksToNs(armGetSystemTick());
		if (now - last_log >= 1000000000ULL)
		{
			log_level(&level, (unsigned)((now - start) / 1000000000ULL));
			last_log = now;
		}
		if (now - last_list >= PROBE_LIST_EVERY * 1000000000ULL)
		{
			list_inputs();
			host_logf(HOST_LOG_INFO, "mic probe: the input's state %d", (int)audinGetDeviceState());
			last_list = now;
		}
		if (now - start >= PROBE_SECONDS * 1000000000ULL)
			break;
	}
	audinStopAudioIn();

done:
	audinExit();
	for (index = 0; index < PROBE_BUFFERS; index++)
		free(data[index]);
	host_logf(HOST_LOG_INFO, "mic probe: done");
	return NULL;
}

void host_mic_probe_start(void)
{
	pthread_t thread;

	if (pthread_create(&thread, NULL, probe_thread, NULL) != 0)
	{
		host_logf(HOST_LOG_ERROR, "mic probe: its thread could not be made");
		return;
	}
	pthread_detach(thread);
}
