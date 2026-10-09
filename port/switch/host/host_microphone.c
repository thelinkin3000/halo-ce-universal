/*
HOST_MICROPHONE.C

Voice chat's microphone on the Switch (port/linux/src/voice_audio.c reads it
as an SDL3 stream without a callback: host_sdl2.c's host_sdl_open_audio_stream
sends that one here). The Switch's SDL2 cannot record, so it is libnx's
audin: its one input, "BuiltInHeadset", a headset in the console's jack.

What debug.mic_probe found (host_mic_probe.c) shapes this: the input gives
48 kHz, two channels of 16 bits, in buffers that come back 50 a second; a
headset plugged in or out changes nothing to open again, but with none in
the jack the buffers come back all zeros and twice as fast. So a thread
keeps MICROPHONE_BUFFERS buffers queued, drops the buffers of zeros (voice
chat then hears nothing, as with no microphone), takes the first channel
of the others (the one the probe heard the voice on: averaging it with a
second that is silent would halve it, under open mic's level) as the 48 kHz
mono floats voice chat asked for, and keeps
them in a ring the guest reads (the oldest dropped when it is full: a voice
must not play late). audin's own state says "stopped" while it captures, so
it is not read.
*/

#include "host.h"

#include <switch.h>

#include <malloc.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define MICROPHONE_RATE 48000
/* 20 ms buffers, as voice chat's frames are long */
#define MICROPHONE_FRAME_SAMPLES 960
#define MICROPHONE_BUFFERS 4
/* a second of mono samples kept for the guest */
#define MICROPHONE_RING_SAMPLES MICROPHONE_RATE

static struct
{
	int open;
	volatile int stopping;
	pthread_t thread;
	pthread_mutex_t lock;
	u32 channels;
	AudioInBuffer buffers[MICROPHONE_BUFFERS];
	u8 *data[MICROPHONE_BUFFERS];
	u32 data_size;
	/* the mono samples (under lock) */
	float ring[MICROPHONE_RING_SAMPLES];
	int read, count;
	/* whether the input last gave sound, for saying when it changes */
	int hearing;
} microphone = { .lock = PTHREAD_MUTEX_INITIALIZER };

static void keep(const AudioInBuffer *buffer)
{
	const s16 *samples = buffer->buffer;
	u32 channels = microphone.channels;
	int frames = (int)(buffer->data_size / 2 / channels), frame, any = 0;

	for (frame = 0; frame < frames * (int)channels && !any; frame++)
		any = samples[frame] != 0;
	if (any != microphone.hearing)
	{
		microphone.hearing = any;
		host_logf(HOST_LOG_INFO, any ? "microphone: hearing the headset" :
			"microphone: no sound (no headset in the jack?)");
	}
	if (!any)
		return;

	pthread_mutex_lock(&microphone.lock);
	for (frame = 0; frame < frames; frame++)
	{
		float value = samples[frame * channels] / 32768.0f;
		int at;

		if (microphone.count == MICROPHONE_RING_SAMPLES)
		{
			microphone.read = (microphone.read + 1) % MICROPHONE_RING_SAMPLES;
			microphone.count--;
		}
		at = (microphone.read + microphone.count) % MICROPHONE_RING_SAMPLES;
		microphone.ring[at] = value;
		microphone.count++;
	}
	pthread_mutex_unlock(&microphone.lock);
}

static void *capture_thread(void *unused)
{
	(void)unused;
	host_thread_place_on_helper_core();
	while (!microphone.stopping)
	{
		AudioInBuffer *released = NULL;
		u32 released_count = 0;
		Result result;

		/* (a tenth of a second at most, so a close is not kept waiting) */
		result = audinWaitCaptureFinish(&released, &released_count, 100000000ULL);
		while (R_SUCCEEDED(result) && released)
		{
			keep(released);
			released->data_size = microphone.data_size;
			audinAppendAudioInBuffer(released);
			released = NULL;
			released_count = 0;
			result = audinGetReleasedAudioInBuffer(&released, &released_count);
			if (!released_count)
				break;
		}
	}
	return NULL;
}

static void release(void)
{
	int index;

	for (index = 0; index < MICROPHONE_BUFFERS; index++)
	{
		free(microphone.data[index]);
		microphone.data[index] = NULL;
	}
}

int host_microphone_open(void)
{
	u32 buffer_size;
	Result result;
	int index;

	if (microphone.open)
	{
		host_logf(HOST_LOG_ERROR, "microphone: already open");
		return 0;
	}
	result = audinInitialize();
	if (R_FAILED(result))
	{
		host_logf(HOST_LOG_ERROR, "microphone: audinInitialize failed: 0x%x", (unsigned)result);
		return 0;
	}
	microphone.channels = audinGetChannelCount();
	if (audinGetPcmFormat() != PcmFormat_Int16 || !microphone.channels ||
		audinGetSampleRate() != MICROPHONE_RATE)
	{
		host_logf(HOST_LOG_ERROR, "microphone: the input gives %u Hz, %u channel(s), format %d; voice chat "
			"needs 48000 Hz of 16-bit samples", (unsigned)audinGetSampleRate(), (unsigned)microphone.channels,
			(int)audinGetPcmFormat());
		audinExit();
		return 0;
	}
	microphone.data_size = MICROPHONE_FRAME_SAMPLES * microphone.channels * 2;
	buffer_size = (microphone.data_size + 0xfff) & ~0xfffu;
	for (index = 0; index < MICROPHONE_BUFFERS; index++)
	{
		microphone.data[index] = memalign(0x1000, buffer_size);
		if (!microphone.data[index])
		{
			host_logf(HOST_LOG_ERROR, "microphone: out of memory for its buffers");
			release();
			audinExit();
			return 0;
		}
		memset(microphone.data[index], 0, buffer_size);
		microphone.buffers[index].next = NULL;
		microphone.buffers[index].buffer = microphone.data[index];
		microphone.buffers[index].buffer_size = buffer_size;
		microphone.buffers[index].data_size = microphone.data_size;
		microphone.buffers[index].data_offset = 0;
	}
	result = audinStartAudioIn();
	if (R_FAILED(result))
	{
		host_logf(HOST_LOG_ERROR, "microphone: audinStartAudioIn failed: 0x%x", (unsigned)result);
		release();
		audinExit();
		return 0;
	}
	for (index = 0; index < MICROPHONE_BUFFERS; index++)
		audinAppendAudioInBuffer(&microphone.buffers[index]);

	pthread_mutex_lock(&microphone.lock);
	microphone.read = microphone.count = 0;
	pthread_mutex_unlock(&microphone.lock);
	microphone.hearing = -1;
	microphone.stopping = 0;
	if (pthread_create(&microphone.thread, NULL, capture_thread, NULL) != 0)
	{
		host_logf(HOST_LOG_ERROR, "microphone: its thread could not be made");
		audinStopAudioIn();
		release();
		audinExit();
		return 0;
	}
	microphone.open = 1;
	host_logf(HOST_LOG_INFO, "microphone: open (%u channels of 16 bits at %u Hz; the first is read)",
		(unsigned)microphone.channels, (unsigned)MICROPHONE_RATE);
	return 1;
}

/* the bytes of mono floats there are to read */
int host_microphone_available(void)
{
	int bytes;

	pthread_mutex_lock(&microphone.lock);
	bytes = microphone.count * (int)sizeof(float);
	pthread_mutex_unlock(&microphone.lock);
	return bytes;
}

/* up to length bytes of them (whole samples), into data; how many */
int host_microphone_read(void *data, int length)
{
	float *out = data;
	int wanted = length / (int)sizeof(float), taken;

	if (wanted <= 0)
		return 0;
	pthread_mutex_lock(&microphone.lock);
	for (taken = 0; taken < wanted && microphone.count; taken++)
	{
		out[taken] = microphone.ring[microphone.read];
		microphone.read = (microphone.read + 1) % MICROPHONE_RING_SAMPLES;
		microphone.count--;
	}
	pthread_mutex_unlock(&microphone.lock);
	return taken * (int)sizeof(float);
}

void host_microphone_close(void)
{
	if (!microphone.open)
		return;
	microphone.stopping = 1;
	pthread_join(microphone.thread, NULL);
	audinStopAudioIn();
	audinExit();
	release();
	microphone.open = 0;
	host_logf(HOST_LOG_INFO, "microphone: closed");
}
