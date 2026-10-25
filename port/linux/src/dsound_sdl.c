/*
DSOUND_SDL.C

Xbox DirectSound for the Linux build: a software mixer on an SDL3 audio
stream.

The game plays everything through DirectSound streams: 16-bit stereo PCM
(music and other uncompressed sounds) and Xbox ADPCM, mono or stereo, at 22
or 44 kHz. A packet is decoded to 16-bit PCM when the game submits it, since
the sound cache may reuse its memory once the packet completes. The mixer
runs on SDL's audio thread; for every voice it resamples to the output rate
with a windowed sinc low pass (which is how SetFrequency changes pitch;
resampling) and applies:
	- the stream volume (millibels),
	- the front left and right mix bin volumes of 2D voices,
	- for 3D voices, DirectSound's inverse distance rolloff between the
	  minimum and maximum distance, an equal power pan from the source's
	  direction in listener space, and the I3DL2 direct path, obstruction
	  and occlusion levels, their high frequency levels a low pass at the
	  environment's HF reference: a sound behind a wall is muffled;
	- for 3D voices, DirectSound's Doppler shift from the velocities the
	  game gives the voice and the listener (doppler).
3D voices also send to an I3DL2 reverb of the environment the listener is in
(reverb). Cones are not modelled (no sound of the maps has one). A look-ahead
limiter keeps the sum under full scale (limit). In the VR mode, with vr.hrtf,
a mono 3D voice goes to each ear through a model of a head instead of the pan
(hrtf).

The output is stereo or 5.1 (audio.channels; "auto": 5.1 on a device of
six channels or more, as the host's SDL reports it, output_channels_choose).
For 5.1 the speaker config reports AC-3, as an Xbox set to Dolby Digital
does: the game then sends its 2D voices to the center and back mix bins too
(dsound_initialize_channel: a mono one at half to the front three, a stereo
one whole to the front pair and at half to the back pair), 3D voices and
stereo voices positioned in the world are panned around the five full range
speakers by VBAP (surround_pan), never the HRTF, and the reverb plays from the
front and back pairs. The mix bins 0-5 are in SDL's 5.1 order (FL, FR, FC,
LFE, BL, BR), so bin n is output channel n. The game sends nothing to the
LFE bin. Stereo output mixes exactly as before.

Packets the mixer has finished are completed from DirectSoundDoWork, which
the game calls every frame, and from Flush, never from the audio thread:
the game's completion callback is not meant to run concurrently with it.

Without an audio device, a clock thread runs the same mixer into a scratch
buffer, so streams still drain at their real rate.

audio.volume sets the master volume (default 1.0); audio.reverb = false
turns the reverb off, and audio.doppler = false the Doppler shift;
audio.resampler = "linear" resamples between neighbouring frames instead of
with the sinc low pass; audio.enabled = false skips opening a device;
audio.channels chooses stereo or 5.1
(port_config.c). For automated tests, debug.audio_record records the mix to a
WAV file (its front pair, on 5.1) and debug.audio_log logs the 3D voices' places
and gains (record, voices_log).
*/

#include "platform.h"
#include "sdl_platform.h"
#include "port_config.h"
#include "voice_audio.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define OUTPUT_RATE 48000
#define OUTPUT_CHANNELS 2 /* the stereo mix's, and the reverb's outputs */
#define MAXIMUM_OUTPUT_CHANNELS 6
#define MAXIMUM_STREAM_PACKETS 64
#define MIX_CHUNK_FRAMES 1024

#define XBOX_ADPCM_BLOCK_BYTES 36
#define XBOX_ADPCM_BLOCK_SAMPLES 64

/* the resampler (resampling) */
#define RESAMPLER_ZERO_CROSSINGS 24
#define RESAMPLER_TABLE_STEPS 256
#define RESAMPLER_CUTOFF 0.96
#define RESAMPLER_KAISER_BETA 6.5
#define RESAMPLER_MAXIMUM_STRETCH 2
#define RESAMPLER_HISTORY 128

/* the Doppler shift (doppler): the speed of sound (metres a second), the
Doppler factor (DirectSound's default, which the game keeps), and the most a
voice is shifted either way (an octave) */
#define DOPPLER_SPEED_OF_SOUND 343.0f
#define DOPPLER_FACTOR 1.0f
#define DOPPLER_MAXIMUM_RATIO 2.0f

/* the HRTF (hrtf): the frames a voice keeps for its ears' delays, the head's
radius (metres) and the speed of sound (metres a second), the head shadow's
alpha over all directions (hrtf_initialize), the distance within which a
sound's direction fades out (world units), the pinna's notch (its frequency
from below the ears to above them, its depth and its Q) and the shadow of
the pinna behind (its frequency and its depth) */
#define HRTF_FRAMES 64
#define HRTF_HEAD_RADIUS 0.0875f
#define HRTF_SPEED_OF_SOUND 343.0f
#define HRTF_DIFFUSE_ALPHA 1.03f
#define HRTF_NEAR 0.25f
#define HRTF_NOTCH_LOW 6000.0f
#define HRTF_NOTCH_HIGH 11000.0f
#define HRTF_NOTCH_DECIBELS -10.0f
#define HRTF_NOTCH_Q 2.0f
#define HRTF_REAR_FREQUENCY 2500.0f
#define HRTF_REAR_DEPTH 0.55f
#if defined(__GNUC__) || defined(__clang__)
#define RESAMPLER_ALIGNED __attribute__((aligned(16)))
#else
#define RESAMPLER_ALIGNED
#endif

/* ---------- voices */

struct voice_packet
{
	XMEDIAPACKET packet;
	short *samples;           /* interleaved, source channel count */
	unsigned long frames;
	BOOL finished;            /* played out by the mixer, not yet completed */
};

/* a mono 3D voice's way to each ear through the HRTF (hrtf_place), which
the mixer ramps from one mix to the next as it does the gains */
struct hrtf_parameters
{
	/* each ear's delay (frames; left, right), and its head shadow, a one-pole
	one-zero filter's zero (its pole is the same for all: hrtf) */
	float delay[2];
	float shadow_b0[2], shadow_b1[2];
	/* the pinna's notch (a peaking filter's b0, b1, b2, a1, a2) at its
	frequency, and how much of the high frequencies a sound behind loses
	(0 to 1) */
	float notch[5];
	float notch_frequency;
	float rear;
};

struct sdl_stream
{
	/* must be first: in C an IDirectSoundStream is just { lpVtbl } */
	IDirectSoundStream object;
	struct sdl_stream *next;
	ULONG reference_count;
	LPFNXMEDIAOBJECTCALLBACK callback;
	LPVOID context;

	/* format */
	BOOL adpcm;
	unsigned long channels;
	DWORD sample_rate;
	DWORD frequency;

	BOOL paused;

	/* 2D gains */
	float volume;             /* SetVolume */
	float mix_left, mix_right;
	/* a stereo voice of a sound in the world: panned towards it, -1 left
	to 1 right, its distance, and the fade of its volume with distance that
	the game made (dsound_sdl_stream_set_stereo_position) */
	BOOL stereo_positioned;
	float stereo_pan;
	float stereo_distance;
	float stereo_distance_fade;
	float headroom;
	/* on 5.1 (output_channels 6): the gains from each source channel to
	each speaker mix bin (SetMixBins, SetMixBinVolumes) */
	float surround_bins[2][MAXIMUM_OUTPUT_CHANNELS];

	/* 3D */
	BOOL has_3d;
	DWORD mode;
	float position[3];
	/* (units a second; SetVelocity) */
	float velocity[3];
	float minimum_distance, maximum_distance;
	/* the I3DL2 direct path's level and high frequency level, in millibels
	with obstruction and occlusion (SetI3DL2Source) */
	LONG direct, direct_hf;
	/* the I3DL2 room (reverb) send's level and high frequency level, in
	millibels with occlusion, and its rolloff factor (SetI3DL2Source) */
	LONG room, room_hf;
	float room_rolloff_factor;

	struct voice_packet packets[MAXIMUM_STREAM_PACKETS];
	unsigned long packet_head;
	unsigned long packet_count;
	/* the next frame to take from the first packet not finished */
	unsigned long cursor;
	/* the resampler's (resampler_reset): the last RESAMPLER_HISTORY frames
	taken, each channel's apart and each frame twice, RESAMPLER_HISTORY apart,
	so that the frames a low pass reaches are side by side however the ring
	wraps (resampler_dot); how many were taken, the frame the output is at and
	how far past it, and the frames of silence taken since the packets ran
	out */
	float history[2][2 * RESAMPLER_HISTORY];
	unsigned long history_count;
	unsigned long center;
	double phase;
	unsigned long silence;
	/* gains and low pass coefficients the mixer is ramping from, to avoid
	clicks, and the step through the frames (the frequency, Doppler shifted,
	over the output rate) */
	float current_left, current_right, current_room;
	double current_step;
	float current_direct_lowpass, current_room_lowpass;
	float current_surround[2][MAXIMUM_OUTPUT_CHANNELS];
	BOOL gains_valid;
	/* the direct path's low pass, for each channel, and the room send's */
	float direct_lowpass[2];
	float room_lowpass;
	/* through the HRTF (hrtf): the parameters the mixer is ramping from, the
	voice's last HRTF_FRAMES frames as the ears take them (its direct path's,
	notched; how many were written), and the filters' states: the rear's low
	pass, the notch's last two inputs and outputs, each ear's shadow's last
	input and output */
	BOOL hrtf_on;
	struct hrtf_parameters hrtf_current;
	float hrtf_frames[HRTF_FRAMES];
	unsigned long hrtf_written;
	float hrtf_rear;
	float hrtf_notch[4];
	float hrtf_shadow[2][2];
};

static pthread_mutex_t mixer_lock = PTHREAD_MUTEX_INITIALIZER;
static struct sdl_stream *streams;

/* the listener, in DirectSound's left-handed +y up space */
static struct
{
	float position[3];
	float front[3];
	float top[3];
	float rolloff_factor;
	/* meters a unit (SetDistanceFactor): the game sets 3.048, a world unit
	being 10 feet. Only the Doppler shift uses it (doppler) */
	float distance_factor;
	/* (units a second; the game gives each voice's velocity relative to the
	listener, and the listener none) */
	float velocity[3];
} listener = { { 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, 1.0f, 1.0f, { 0, 0, 0 } };

static float master_volume = 1.0f;
/* audio.doppler */
static BOOL doppler_enabled = TRUE;
/* vr.hrtf, in the VR mode (DirectSoundDoWork) */
static BOOL hrtf_enabled = FALSE;
/* 2 or 6 (audio.channels), chosen when the device first opens; the game
sets its mix bins by it, so it holds while the game runs */
static unsigned long output_channels = 2;

/* the I3DL2 listener properties (SetI3DL2Listener), which the reverb models:
DirectSound's defaults until the game sets an environment, their room level
leaving it silent; serial counts the changes */
static DSI3DL2LISTENER environment =
{
	DSBVOLUME_MIN, 0, 0.0f, 1.49f, 0.83f, -2602, 0.007f, 200, 0.011f, 100.0f, 100.0f, 5000.0f
};
static unsigned long environment_serial;
/* audio.reverb */
static BOOL reverb_enabled = TRUE;

static float gain_from_millibels(LONG millibels)
{
	if (millibels <= DSBVOLUME_MIN)
		return 0.0f;
	return powf(10.0f, (float)millibels / 2000.0f);
}

/* value, or 0 once it is far too small to hear: the state of a filter or of
the reverb fed silence decays into the denormals, which many CPUs work out
slowly, and neither x86 nor ARM flushes them to zero by default */
static float flush_denormal(float value)
{
	return fabsf(value) < 1.0e-20f ? 0.0f : value;
}

/* cos(2 pi f / OUTPUT_RATE) of a filter's frequency f, within what the
output rate holds */
static float frequency_cosine(float frequency)
{
	if (frequency < 20.0f)
		frequency = 20.0f;
	if (frequency > 0.45f * OUTPUT_RATE)
		frequency = 0.45f * OUTPUT_RATE;
	return cosf(2.0f * 3.14159265f * frequency / OUTPUT_RATE);
}

/* the coefficient a of the one-pole low pass y = x + a (y' - x) whose gain is
gain (at most 1) at the frequency of cosine (frequency_cosine) */
static float lowpass_coefficient(float gain, float cosine)
{
	float a, b;

	if (gain >= 1.0f)
		return 0.0f;
	if (gain < 0.001f)
		gain = 0.001f;
	a = 1.0f - gain * gain;
	b = 1.0f - gain * gain * cosine;
	return (b - sqrtf(b * b - a * a)) / a;
}

/* ---------- decoding */

static const int ima_index_table[16] =
{
	-1, -1, -1, -1, 2, 4, 6, 8,
	-1, -1, -1, -1, 2, 4, 6, 8,
};

static const int ima_step_table[89] =
{
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
	50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
	253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
	1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
	3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
	11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
	32767,
};

static int ima_expand(int nibble, int *predictor, int *index)
{
	int step = ima_step_table[*index];
	int difference = step >> 3;

	if (nibble & 1) difference += step >> 2;
	if (nibble & 2) difference += step >> 1;
	if (nibble & 4) difference += step;
	if (nibble & 8) difference = -difference;
	*predictor += difference;
	if (*predictor > 32767) *predictor = 32767;
	if (*predictor < -32768) *predictor = -32768;
	*index += ima_index_table[nibble];
	if (*index < 0) *index = 0;
	if (*index > 88) *index = 88;
	return *predictor;
}

/* Xbox ADPCM: per block, a 4-byte header per channel (first sample, step
index), then 4-byte groups of eight nibbles, low nibble first, alternating
between channels; 64 samples per channel. The header's sample is the block's
first, and the nibbles code the 63 after it: the 64th nibble only pads the
block (the maps' sounds always have 0 there), and decoding it in place of the
header's sample put a wrong sample in every 64, a buzz at 344 Hz in 22 kHz
sounds. */
static short *decode_adpcm(const unsigned char *source, unsigned long size, unsigned long channels,
	unsigned long *frame_count)
{
	unsigned long block_bytes = XBOX_ADPCM_BLOCK_BYTES * channels;
	unsigned long blocks = size / block_bytes;
	short *samples = malloc((blocks ? blocks : 1) * XBOX_ADPCM_BLOCK_SAMPLES * channels * sizeof(short));
	unsigned long block, channel;

	if (!samples)
	{
		*frame_count = 0;
		return NULL;
	}
	for (block = 0; block < blocks; block++)
	{
		const unsigned char *data = source + block * block_bytes;
		short *output = samples + block * XBOX_ADPCM_BLOCK_SAMPLES * channels;

		for (channel = 0; channel < channels; channel++)
		{
			const unsigned char *header = data + channel * 4;
			int predictor = (short)(header[0] | (header[1] << 8));
			int index = header[2] > 88 ? 88 : header[2];
			unsigned long group, byte;

			output[channel] = (short)predictor;
			for (group = 0; group < 8; group++)
			{
				const unsigned char *nibbles = data + 4 * channels + (group * channels + channel) * 4;

				for (byte = 0; byte < 4; byte++)
				{
					/* nibble n codes sample n + 1 */
					unsigned long sample = group * 8 + byte * 2 + 1;

					output[sample * channels + channel] = (short)ima_expand(nibbles[byte] & 0xf, &predictor, &index);
					if (sample + 1 < XBOX_ADPCM_BLOCK_SAMPLES)
						output[(sample + 1) * channels + channel] = (short)ima_expand(nibbles[byte] >> 4, &predictor, &index);
				}
			}
		}
	}
	*frame_count = blocks * XBOX_ADPCM_BLOCK_SAMPLES;
	return samples;
}

static short *decode_pcm(const unsigned char *source, unsigned long size, unsigned long channels,
	unsigned long *frame_count)
{
	unsigned long frames = size / (2 * channels);
	short *samples = malloc((frames ? frames : 1) * channels * sizeof(short));

	if (samples)
		memcpy(samples, source, frames * channels * sizeof(short));
	*frame_count = samples ? frames : 0;
	return samples;
}

/* ---------- 3D */

static float dot3(const float *a, const float *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* DirectSound's inverse distance law, held beyond the maximum distance. The
game sets a voice's maximum distance before its minimum, so the maximum can
be under the minimum for a moment: it is held at the minimum */
static float distance_attenuation(const struct sdl_stream *stream, float distance, float rolloff_factor)
{
	float minimum = stream->minimum_distance;
	float maximum = stream->maximum_distance > minimum ? stream->maximum_distance : minimum;

	if (minimum <= 0.0f || distance <= minimum)
		return 1.0f;
	if (distance > maximum)
		distance = maximum;
	return minimum / (minimum + rolloff_factor * (distance - minimum));
}

/* ---------- Doppler

The game gives each 3D voice its velocity (units a second, relative to the
listener: sound_manager.c update_channels) and the Xbox's DirectSound shifted
its pitch by them, as a sound passing by is heard: a Banshee, a Ghost or a
plasma bolt rising as it comes and falling as it goes. DirectSound's shift:
the frequency times (c + D v_listener) / (c + D v_source), c the speed of
sound, D the Doppler factor, the velocities' parts along the line from the
listener to the source in metres a second (the distance factor's metres a
unit), the source's away from the listener, the listener's towards the
source. Each part is held within half the speed of sound, and the shift to
an octave either way, so that a projectile passing close by, or a velocity
that jumps (a teleport), takes no voice up out of hearing. */

static float doppler_ratio(const struct sdl_stream *stream)
{
	float offset[3], distance, source_speed, listener_speed = 0.0f, ratio, limit = 0.5f * DOPPLER_SPEED_OF_SOUND;
	int axis;

	if (!doppler_enabled || !stream->has_3d || stream->mode == DS3DMODE_DISABLE)
		return 1.0f;
	for (axis = 0; axis < 3; axis++)
	{
		offset[axis] = stream->position[axis] -
			(stream->mode == DS3DMODE_HEADRELATIVE ? 0.0f : listener.position[axis]);
	}
	distance = sqrtf(dot3(offset, offset));
	if (distance < 1.0e-4f)
		return 1.0f;
	source_speed = DOPPLER_FACTOR * listener.distance_factor * dot3(stream->velocity, offset) / distance;
	/* (head-relative: the voice's velocity is the listener's already) */
	if (stream->mode != DS3DMODE_HEADRELATIVE)
		listener_speed = DOPPLER_FACTOR * listener.distance_factor * dot3(listener.velocity, offset) / distance;
	source_speed = source_speed < -limit ? -limit : source_speed > limit ? limit : source_speed;
	listener_speed = listener_speed < -limit ? -limit : listener_speed > limit ? limit : listener_speed;
	ratio = (DOPPLER_SPEED_OF_SOUND + listener_speed) / (DOPPLER_SPEED_OF_SOUND + source_speed);
	if (ratio > DOPPLER_MAXIMUM_RATIO)
		ratio = DOPPLER_MAXIMUM_RATIO;
	if (ratio < 1.0f / DOPPLER_MAXIMUM_RATIO)
		ratio = 1.0f / DOPPLER_MAXIMUM_RATIO;
	return ratio;
}

/* ---------- the HRTF

In the VR mode (vr.hrtf), a mono 3D voice reaches each ear as it would a
head's, from its direction (the listener's: the head as it is tracked,
sound_manager.c refresh_listener), rather than panned between them: a
sound behind or above is told from one ahead or below, on headphones or the
headset's speakers. Not a measured HRTF (which would need a dataset of a
head's, and a convolution for each voice and ear) but a model of a head's
(Brown and Duda, "A structural model for binaural sound synthesis", 1998),
a few filters a voice, its parameters from the direction:
	- each ear its own delay, from the time the sound takes around a sphere
	  of a head's size (Woodworth's), to the far ear up to 0.66 ms later;
	- each ear the head's shadow, a one-pole one-zero shelf above about 1
	  kHz, from 6 dB louder facing the sound to 20 dB quieter in the shadow
	  (Brown and Duda's), the louder and quieter about what a sound from
	  everywhere has (the diffuse field: HRTF_DIFFUSE_ALPHA), so that over
	  all directions the sound is no brighter nor duller than panned;
	- the pinna's notch, its frequency rising with the sound from below the
	  ears (6 kHz) to above them (11 kHz), as a pinna's first notch does
	  (it is what tells up from down);
	- a sound behind loses some of its high frequencies (the pinna's shadow,
	  up to 7 dB above 2.5 kHz).
A sound close to the head (within HRTF_NEAR) fades to the middle, as the pan
centres one within its minimum distance. The parameters are ramped across
each mix, as the gains are: a turn of the head moves the sound smoothly
from ear to ear, with no step. */

static struct
{
	/* the head shadow's pole (a1 of y = b0 x + b1 x' - a1 y', the same for
	every direction) and the bilinear transform's constant over the pole;
	the rear's low pass's coefficient */
	float shadow_a1;
	float shadow_k;
	float rear_lowpass;
} hrtf;

static void hrtf_initialize(void)
{
	/* Brown and Duda's pole, 2c/a, lowered by the diffuse field's alpha:
	the shadow over the diffuse field's (H(alpha) / H(alpha_diffuse)) */
	float pole = 2.0f * HRTF_SPEED_OF_SOUND / (HRTF_HEAD_RADIUS * HRTF_DIFFUSE_ALPHA);

	hrtf.shadow_k = 1.0f / tanf(pole / (2.0f * OUTPUT_RATE));
	hrtf.shadow_a1 = (1.0f - hrtf.shadow_k) / (1.0f + hrtf.shadow_k);
	hrtf.rear_lowpass = expf(-2.0f * 3.14159265f * HRTF_REAR_FREQUENCY / OUTPUT_RATE);
}

/* the HRTF's parameters for a sound right, up and ahead of the head, as
far away (world units) */
static void hrtf_place(float right, float up, float ahead, float distance, struct hrtf_parameters *parameters)
{
	float strength = distance < HRTF_NEAR ? distance / HRTF_NEAR : 1.0f;
	float lateral = 0.0f, rise = 0.0f, front = 1.0f, delay[2], elevation, frequency, a, w, alpha, a0;
	int ear;

	if (distance > 1.0e-4f)
	{
		lateral = right / distance;
		rise = up / distance;
		front = ahead / distance;
	}
	for (ear = 0; ear < 2; ear++)
	{
		/* the angle from the ear's axis: Brown and Duda's alpha, 2 facing
		the sound, 0.1 at 150 degrees from it */
		float cosine = ear ? lateral : -lateral;
		float theta = acosf(cosine < -1.0f ? -1.0f : (cosine > 1.0f ? 1.0f : cosine));
		float shadow = 1.0f + strength *
			((1.05f + 0.95f * cosf(theta * (180.0f / 150.0f))) / HRTF_DIFFUSE_ALPHA - 1.0f);

		parameters->shadow_b0[ear] = (1.0f + shadow * hrtf.shadow_k) / (1.0f + hrtf.shadow_k);
		parameters->shadow_b1[ear] = (1.0f - shadow * hrtf.shadow_k) / (1.0f + hrtf.shadow_k);
		/* Woodworth's: the way around the head to the ear */
		delay[ear] = theta < 0.5f * 3.14159265f ? 1.0f - cosf(theta) : theta - 0.5f * 3.14159265f + 1.0f;
	}
	/* (the nearer ear at no delay) */
	for (ear = 0; ear < 2; ear++)
	{
		parameters->delay[ear] = strength * (delay[ear] - (delay[0] < delay[1] ? delay[0] : delay[1])) *
			(HRTF_HEAD_RADIUS / HRTF_SPEED_OF_SOUND) * OUTPUT_RATE;
	}
	/* the notch, from 30 degrees below to 60 above (RBJ's peaking filter) */
	elevation = asinf(rise < -1.0f ? -1.0f : (rise > 1.0f ? 1.0f : rise)) * (180.0f / 3.14159265f);
	elevation = (elevation + 30.0f) / 90.0f;
	frequency = HRTF_NOTCH_LOW + (HRTF_NOTCH_HIGH - HRTF_NOTCH_LOW) *
		(elevation < 0.0f ? 0.0f : (elevation > 1.0f ? 1.0f : elevation));
	a = powf(10.0f, strength * HRTF_NOTCH_DECIBELS / 40.0f);
	w = 2.0f * 3.14159265f * frequency / OUTPUT_RATE;
	alpha = sinf(w) / (2.0f * HRTF_NOTCH_Q);
	a0 = 1.0f + alpha / a;
	parameters->notch[0] = (1.0f + alpha * a) / a0;
	parameters->notch[1] = -2.0f * cosf(w) / a0;
	parameters->notch[2] = (1.0f - alpha * a) / a0;
	parameters->notch[3] = parameters->notch[1];
	parameters->notch[4] = (1.0f - alpha / a) / a0;
	parameters->notch_frequency = frequency;
	parameters->rear = front < 0.0f ? -front * strength * HRTF_REAR_DEPTH : 0.0f;
}

/* the frame of the voice's written ones delay frames before the last
(at least 1: one frame each side of it is read), interpolated (Catmull-Rom) */
static float hrtf_read(const float *frames, unsigned long written, float delay)
{
	unsigned long whole = (unsigned long)delay;
	unsigned long at = written - 1 - whole;
	float t = delay - (float)whole;
	float newer = frames[(at + 1) % HRTF_FRAMES];
	float x0 = frames[at % HRTF_FRAMES];
	float x1 = frames[(at - 1) % HRTF_FRAMES];
	float older = frames[(at - 2) % HRTF_FRAMES];

	return x0 + 0.5f * t * (x1 - newer + t * (2.0f * newer - 5.0f * x0 + 4.0f * x1 - older +
		t * (3.0f * (x0 - x1) + older - newer)));
}

/* parameters moved on by step (a frame's share of a ramp) */
static void hrtf_step(struct hrtf_parameters *parameters, const struct hrtf_parameters *step)
{
	int index;

	for (index = 0; index < 2; index++)
	{
		parameters->delay[index] += step->delay[index];
		parameters->shadow_b0[index] += step->shadow_b0[index];
		parameters->shadow_b1[index] += step->shadow_b1[index];
	}
	for (index = 0; index < 5; index++)
		parameters->notch[index] += step->notch[index];
	parameters->rear += step->rear;
}

/* the gains of a 3D voice to the left and right, and the room rolloff's
attenuation of its room send; through the HRTF (hrtf_on: a mono voice in
the VR mode, vr.hrtf), its gain to each ear and its way there (hrtf_out) */
static void spatialize(const struct sdl_stream *stream, float *left, float *right, float *room,
	BOOL *hrtf_on, struct hrtf_parameters *hrtf_out)
{
	float offset[3], right_axis[3], distance, attenuation, pan, side, ahead, up;
	int axis;

	if (stream->mode == DS3DMODE_HEADRELATIVE)
	{
		for (axis = 0; axis < 3; axis++)
			offset[axis] = stream->position[axis];
		side = offset[0];
		up = offset[1];
		ahead = offset[2];
	}
	else
	{
		for (axis = 0; axis < 3; axis++)
			offset[axis] = stream->position[axis] - listener.position[axis];
		/* left-handed: right = top x front */
		right_axis[0] = listener.top[1] * listener.front[2] - listener.top[2] * listener.front[1];
		right_axis[1] = listener.top[2] * listener.front[0] - listener.top[0] * listener.front[2];
		right_axis[2] = listener.top[0] * listener.front[1] - listener.top[1] * listener.front[0];
		side = dot3(offset, right_axis);
		up = dot3(offset, listener.top);
		ahead = dot3(offset, listener.front);
	}
	/* in the game's units, those of the minimum and maximum distances: the
	distance factor turns units into meters for Doppler, and scaling by it
	here put every 3D sound 3 times as far away, up to 10 dB quieter */
	distance = sqrtf(dot3(offset, offset));
	attenuation = distance_attenuation(stream, distance, listener.rolloff_factor);
	*room = distance_attenuation(stream, distance, environment.flRoomRolloffFactor + stream->room_rolloff_factor);

	/* equal power pan; close sources and sources straight ahead or behind
	stay centred, and neither ear drops below a quarter */
	{
		float horizontal = sqrtf(side * side + ahead * ahead);
		float angle;

		pan = horizontal > 1.0e-4f ? side / horizontal : 0.0f;
		if (distance < stream->minimum_distance && stream->minimum_distance > 0.0f)
			pan *= distance / stream->minimum_distance;
		pan *= 0.75f;
		angle = (pan + 1.0f) * 0.25f * 3.14159265f;
		*left = cosf(angle) * 1.41421356f * 0.70710678f;
		*right = sinf(angle) * 1.41421356f * 0.70710678f;
	}
	/* or through the HRTF, each ear at the pan's gain in the middle (-3
	dB), the head's filters doing the rest */
	*hrtf_on = hrtf_enabled && stream->channels == 1;
	if (*hrtf_on)
	{
		hrtf_place(side, up, ahead, distance, hrtf_out);
		*left = *right = 0.70710678f;
	}
	*left *= attenuation * gain_from_millibels(stream->direct);
	*right *= attenuation * gain_from_millibels(stream->direct);
}

/* the voice's gains to the left and right, and to the reverb (only 3D voices
send to it, as their I3DL2 mix bin did on the Xbox), with the coefficients of
the low passes of its direct path (3D voices: obstruction and occlusion
muffle it) and of the send; a 3D voice through the HRTF (hrtf_on) has its
way to each ear too (hrtf_out) */
static void voice_gains(const struct sdl_stream *stream, float *left, float *right, float *room,
	float *direct_lowpass, float *room_lowpass, BOOL *hrtf_on, struct hrtf_parameters *hrtf_out)
{
	*room = 0.0f;
	*direct_lowpass = 0.0f;
	*room_lowpass = 0.0f;
	*hrtf_on = FALSE;
	if (stream->has_3d && stream->mode != DS3DMODE_DISABLE)
	{
		float rolloff, cosine = frequency_cosine(environment.flHFReference);

		spatialize(stream, left, right, &rolloff, hrtf_on, hrtf_out);
		if (stream->direct_hf < stream->direct)
			*direct_lowpass = lowpass_coefficient(gain_from_millibels(stream->direct_hf - stream->direct), cosine);
		if (reverb_enabled)
		{
			LONG level = environment.lRoom + stream->room;
			LONG high_level = environment.lRoom + environment.lRoomHF + stream->room_hf;

			*room = gain_from_millibels(level) * rolloff;
			if (*room > 0.0f && high_level < level)
				*room_lowpass = lowpass_coefficient(gain_from_millibels(high_level - level), cosine);
		}
	}
	else if (stream->channels == 2 && stream->stereo_positioned)
	{
		/* the equal power pan of a 3D voice, at the gains of a 2D one when
		centred (the game fades it with distance), its direct path muffled
		and its room send made as a 3D voice's are (SetI3DL2Source) */
		float angle = (stream->stereo_pan + 1.0f) * 0.25f * 3.14159265f;
		float direct = gain_from_millibels(stream->direct);
		float cosine = frequency_cosine(environment.flHFReference);

		*left = cosf(angle) * 1.41421356f * stream->mix_left * direct;
		*right = sinf(angle) * 1.41421356f * stream->mix_right * direct;
		if (stream->direct_hf < stream->direct)
			*direct_lowpass = lowpass_coefficient(gain_from_millibels(stream->direct_hf - stream->direct), cosine);
		if (reverb_enabled)
		{
			LONG level = environment.lRoom + stream->room;
			LONG high_level = environment.lRoom + environment.lRoomHF + stream->room_hf;

			/* the room's rolloff with distance, a 3D voice's; the volume
			holds the game's fade with it, which a 3D voice's room send does
			not take, so it is taken back out (no further than a twentieth:
			past that the send fades out with the sound) */
			*room = gain_from_millibels(level) *
				distance_attenuation(stream, stream->stereo_distance,
					environment.flRoomRolloffFactor + stream->room_rolloff_factor) /
				(stream->stereo_distance_fade > 0.05f ? stream->stereo_distance_fade : 0.05f);
			if (*room > 0.0f && high_level < level)
				*room_lowpass = lowpass_coefficient(gain_from_millibels(high_level - level), cosine);
		}
	}
	else
	{
		*left = stream->mix_left;
		*right = stream->mix_right;
	}
	*left *= stream->volume * master_volume;
	*right *= stream->volume * master_volume;
	*room *= stream->volume * master_volume;
}

/* the 5.1 speakers around the listener, clockwise from the center, and
their directions in radians (ITU-R BS.775: fronts at 30 degrees, backs at
110) */
#define SURROUND_RING_SPEAKERS 5
static const unsigned long surround_ring_channel[SURROUND_RING_SPEAKERS] = { 2, 1, 5, 4, 0 };
static const float surround_ring_angle[SURROUND_RING_SPEAKERS + 1] =
{
	0.0f, 0.52359878f, 1.91986218f, 4.36332313f, 5.75958653f, 6.28318531f,
};

/* the speaker gains of a sound from a direction, side (right) and ahead:
vector base amplitude panning (Pulkki's VBAP, in the plane) between the two
speakers either side of it, blended (in power) towards all five equally by
spread, 0 to 1. Their power sums to 1, as the stereo pan's does.

VBAP takes the gains g1 and g2 whose sum g1 l1 + g2 l2 of the speakers'
directions l1 and l2 points the sound's way p, then scales them to a power
of 1: g1 = sin(a2 - a) / sin(a2 - a1) and g2 = sin(a - a1) / sin(a2 - a1),
a the sound's angle, a1 and a2 the speakers', the common divisor gone with
the scaling. So a sound at a speaker plays from it alone, one between two
from both, as the tangent law of stereo panning puts a phantom image (the
listener between them hears it where it is), and no pair is wider than 180
degrees (the backs' 140 is the widest). */
static void surround_pan(float side, float ahead, float spread, float *gains)
{
	float angle = atan2f(side, ahead);
	unsigned long segment, speaker;

	for (speaker = 0; speaker < MAXIMUM_OUTPUT_CHANNELS; speaker++)
		gains[speaker] = 0.0f;
	if (angle < 0.0f)
		angle += 6.28318531f;
	for (segment = 0; segment < SURROUND_RING_SPEAKERS - 1; segment++)
	{
		if (angle < surround_ring_angle[segment + 1])
			break;
	}
	{
		float first = sinf(surround_ring_angle[segment + 1] - angle);
		float second = sinf(angle - surround_ring_angle[segment]);
		float length;

		first = first > 0.0f ? first : 0.0f;
		second = second > 0.0f ? second : 0.0f;
		length = sqrtf(first * first + second * second);
		if (length < 1.0e-6f)
		{
			first = 1.0f;
			length = 1.0f;
		}
		gains[surround_ring_channel[segment]] = first / length;
		gains[surround_ring_channel[(segment + 1) % SURROUND_RING_SPEAKERS]] = second / length;
	}
	for (speaker = 0; speaker < SURROUND_RING_SPEAKERS; speaker++)
	{
		float *gain = &gains[surround_ring_channel[speaker]];

		*gain = sqrtf((1.0f - spread) * *gain * *gain + spread / SURROUND_RING_SPEAKERS);
	}
}

/* on 5.1, a voice's gains from each source channel to each speaker (the
direct path's; its room send and low passes are voice_gains'):
	- a 3D voice is panned around the speakers from its direction, a little
	  spread so that it never vanishes between two, spread out towards all
	  five inside its minimum distance and above or below, as the stereo pan
	  centres those;
	- a stereo voice positioned in the world has its channels' middle (the
	  first row) panned towards it in the front half, and their difference
	  (the second) kept on the front pair as wide as the stereo mix keeps it;
	- a 2D voice goes by its mix bins. */
static void voice_surround_gains(const struct sdl_stream *stream, float gains[2][MAXIMUM_OUTPUT_CHANNELS])
{
	float scale = stream->volume * master_volume;
	unsigned long speaker;

	memset(gains, 0, 2 * sizeof(gains[0]));
	if (stream->has_3d && stream->mode != DS3DMODE_DISABLE)
	{
		float offset[3], right_axis[3], side, ahead, distance, horizontal, attenuation, spread = 0.1f;
		int axis;

		if (stream->mode == DS3DMODE_HEADRELATIVE)
		{
			for (axis = 0; axis < 3; axis++)
				offset[axis] = stream->position[axis];
			side = offset[0];
			ahead = offset[2];
		}
		else
		{
			for (axis = 0; axis < 3; axis++)
				offset[axis] = stream->position[axis] - listener.position[axis];
			right_axis[0] = listener.top[1] * listener.front[2] - listener.top[2] * listener.front[1];
			right_axis[1] = listener.top[2] * listener.front[0] - listener.top[0] * listener.front[2];
			right_axis[2] = listener.top[0] * listener.front[1] - listener.top[1] * listener.front[0];
			side = dot3(offset, right_axis);
			ahead = dot3(offset, listener.front);
		}
		horizontal = sqrtf(side * side + ahead * ahead);
		distance = sqrtf(dot3(offset, offset));
		if (horizontal <= 1.0e-4f)
			spread = 1.0f;
		else if (distance > 1.0e-4f && 0.5f * (1.0f - horizontal / distance) > spread)
			spread = 0.5f * (1.0f - horizontal / distance);
		if (distance < stream->minimum_distance && stream->minimum_distance > 0.0f &&
			1.0f - distance / stream->minimum_distance > spread)
		{
			spread = 1.0f - distance / stream->minimum_distance;
		}
		surround_pan(side, ahead, spread, gains[0]);
		attenuation = distance_attenuation(stream, distance, listener.rolloff_factor) *
			gain_from_millibels(stream->direct);
		for (speaker = 0; speaker < MAXIMUM_OUTPUT_CHANNELS; speaker++)
		{
			gains[0][speaker] *= attenuation;
			/* a stereo 3D voice: both channels from where it is */
			if (stream->channels == 2)
				gains[1][speaker] = gains[0][speaker] *= 0.5f;
		}
	}
	else if (stream->channels == 2 && stream->stereo_positioned)
	{
		float pan = stream->stereo_pan < -1.0f ? -1.0f : stream->stereo_pan > 1.0f ? 1.0f : stream->stereo_pan;
		float angle = (pan + 1.0f) * 0.25f * 3.14159265f;
		float direct = gain_from_millibels(stream->direct);
		float far_gain = (cosf(angle) < sinf(angle) ? cosf(angle) : sinf(angle)) * 1.41421356f;

		/* the middle at the stereo pan's level (1 on each side when centred,
		a power of 2) */
		surround_pan(sinf(pan * 1.57079633f), cosf(pan * 1.57079633f), 0.1f, gains[0]);
		for (speaker = 0; speaker < MAXIMUM_OUTPUT_CHANNELS; speaker++)
			gains[0][speaker] *= 1.41421356f * stream->mix_left * direct;
		gains[1][0] = far_gain * stream->mix_left * direct;
		gains[1][1] = -far_gain * stream->mix_right * direct;
	}
	else
	{
		memcpy(gains, stream->surround_bins, 2 * sizeof(gains[0]));
		if (stream->channels == 1)
			memset(gains[1], 0, sizeof(gains[1]));
	}
	for (speaker = 0; speaker < MAXIMUM_OUTPUT_CHANNELS; speaker++)
	{
		gains[0][speaker] *= scale;
		gains[1][speaker] *= scale;
	}
}

/* ---------- resampling

Each voice is resampled to the output rate by band-limited interpolation (J.
O. Smith's): an output sample is the source frames around its moment, each
weighted by a windowed sinc low pass centred there. The low pass keeps
RESAMPLER_CUTOFF of the source's band (a 22 kHz voice is 0.5 dB down at 10
kHz) and takes the images of it out (80 dB down), but for those of its last
few hundred hertz, which lie beside them, about 20 dB down. Linear
interpolation, which the mixer did before, left the images only 8 to 20 dB
down, a gritty haze above 11 kHz over every 22 kHz voice. A voice
played faster than the output rate takes its frames (a step over 1) gets the
low pass narrowed to match, up to RESAMPLER_MAXIMUM_STRETCH times, so it
does not alias. The frames come from the voice's packets in turn, so the low
pass reads straight across a packet's end into the next. */

/* the low pass's one side, RESAMPLER_TABLE_STEPS values a source frame */
static float resampler_table[RESAMPLER_ZERO_CROSSINGS * RESAMPLER_TABLE_STEPS + 2];
/* the same low pass as the weights of a voice's taps 1 - RESAMPLER_ZERO_CROSSINGS
to RESAMPLER_ZERO_CROSSINGS, at each of RESAMPLER_TABLE_STEPS phases between two
source frames (and one row more, for the last's blend): a voice at the output
rate or slower blends two rows by its phase, which is what the table gave tap
by tap, a third of the work. resampler_deltas holds each row's step to the
next, so the blend is a multiply and an add a tap */
#define RESAMPLER_TAPS (2 * RESAMPLER_ZERO_CROSSINGS)
static float resampler_phases[RESAMPLER_TABLE_STEPS + 1][RESAMPLER_TAPS] RESAMPLER_ALIGNED;
static float resampler_deltas[RESAMPLER_TABLE_STEPS][RESAMPLER_TAPS] RESAMPLER_ALIGNED;

/* how a voice's frames are converted to the output's rate: the windowed sinc
low pass, or between the two frames either side (linear: it lets more of the
images above the source's rate through, which some hear as brighter) */
enum resampler_mode
{
	RESAMPLER_SINC,
	RESAMPLER_LINEAR
};
/* audio.resampler */
static enum resampler_mode resampler_mode = RESAMPLER_SINC;

/* audio.resampler's mode: sinc for any value but "linear" */
static enum resampler_mode resampler_mode_read(void)
{
	const char *setting = config_string("audio.resampler");

	return setting && !strcmp(setting, "linear") ? RESAMPLER_LINEAR : RESAMPLER_SINC;
}

static double bessel_i0(double x)
{
	double sum = 1.0, term = 1.0;
	int k;

	for (k = 1; k < 64 && term > 1.0e-12 * sum; k++)
	{
		term *= (x / (2.0 * k)) * (x / (2.0 * k));
		sum += term;
	}
	return sum;
}

static void resampler_initialize(void)
{
	unsigned long index;

	for (index = 0; index <= RESAMPLER_ZERO_CROSSINGS * RESAMPLER_TABLE_STEPS; index++)
	{
		double distance = (double)index / RESAMPLER_TABLE_STEPS;
		double edge = distance / RESAMPLER_ZERO_CROSSINGS;
		double angle = 3.14159265358979 * RESAMPLER_CUTOFF * distance;
		double sinc = index ? sin(angle) / angle : 1.0;
		double window = bessel_i0(RESAMPLER_KAISER_BETA * sqrt(1.0 - edge * edge)) / bessel_i0(RESAMPLER_KAISER_BETA);

		resampler_table[index] = (float)(RESAMPLER_CUTOFF * sinc * window);
	}
	resampler_table[RESAMPLER_ZERO_CROSSINGS * RESAMPLER_TABLE_STEPS + 1] = 0.0f;
}

/* the low pass, distance source frames from its centre times
RESAMPLER_TABLE_STEPS */
static float resampler_weight(float distance)
{
	unsigned long index = (unsigned long)distance;
	float fraction;

	if (index >= RESAMPLER_ZERO_CROSSINGS * RESAMPLER_TABLE_STEPS)
		return 0.0f;
	fraction = distance - (float)index;
	return resampler_table[index] + (resampler_table[index + 1] - resampler_table[index]) * fraction;
}

static void resampler_phases_initialize(void)
{
	unsigned long phase, tap;

	for (phase = 0; phase <= RESAMPLER_TABLE_STEPS; phase++)
	{
		for (tap = 0; tap < 2 * RESAMPLER_ZERO_CROSSINGS; tap++)
		{
			float distance = fabsf((float)((long)tap + 1 - RESAMPLER_ZERO_CROSSINGS) -
				(float)phase / RESAMPLER_TABLE_STEPS);

			resampler_phases[phase][tap] = resampler_weight(distance * RESAMPLER_TABLE_STEPS);
		}
	}
	for (phase = 0; phase < RESAMPLER_TABLE_STEPS; phase++)
	{
		for (tap = 0; tap < RESAMPLER_TAPS; tap++)
			resampler_deltas[phase][tap] = resampler_phases[phase + 1][tap] - resampler_phases[phase][tap];
	}
}

/* The low pass over a voice's frames, the sum of each frame times its weight:
four taps at a time where the compiler has vectors (SSE, NEON), summed in four
lanes, then the lanes together. Only the order of the additions differs from
tap by tap, a difference of the order of float's rounding (-115 dB or less);
RESAMPLER_SCALAR adds them tap by tap (tools/harness/tests/test_mixer.py). */
#if (defined(__GNUC__) || defined(__clang__)) && !defined(RESAMPLER_SCALAR)
typedef float resampler_vector __attribute__((vector_size(16)));
typedef float resampler_unaligned_vector __attribute__((vector_size(16), aligned(4)));
/* (each row of the phase tables whole vectors, aligned) */
typedef char resampler_taps_in_fours[RESAMPLER_TAPS % 4 ? -1 : 1];
#define RESAMPLER_LANES 4
#else
#define RESAMPLER_LANES 1
#endif

/* frames[0, RESAMPLER_TAPS) weighted by the phase table's row blended blend of
the way to the next */
static float resampler_dot(const float *frames, const float *weights, const float *deltas, float blend)
{
#if RESAMPLER_LANES == 4
	resampler_vector sum = { 0.0f, 0.0f, 0.0f, 0.0f }, blends = { blend, blend, blend, blend };
	int tap;

	for (tap = 0; tap < RESAMPLER_TAPS; tap += 4)
	{
		resampler_vector weight = *(const resampler_vector *)(weights + tap) +
			*(const resampler_vector *)(deltas + tap) * blends;

		sum += *(const resampler_unaligned_vector *)(frames + tap) * weight;
	}
	return (sum[0] + sum[1]) + (sum[2] + sum[3]);
#else
	float sum = 0.0f;
	int tap;

	for (tap = 0; tap < RESAMPLER_TAPS; tap++)
		sum += frames[tap] * (weights[tap] + deltas[tap] * blend);
	return sum;
#endif
}

/* resampler_dot of two channels' frames, with the same weights */
static void resampler_dot2(const float *left_frames, const float *right_frames, const float *weights,
	const float *deltas, float blend, float *left, float *right)
{
#if RESAMPLER_LANES == 4
	resampler_vector left_sum = { 0.0f, 0.0f, 0.0f, 0.0f }, right_sum = left_sum;
	resampler_vector blends = { blend, blend, blend, blend };
	int tap;

	for (tap = 0; tap < RESAMPLER_TAPS; tap += 4)
	{
		resampler_vector weight = *(const resampler_vector *)(weights + tap) +
			*(const resampler_vector *)(deltas + tap) * blends;

		left_sum += *(const resampler_unaligned_vector *)(left_frames + tap) * weight;
		right_sum += *(const resampler_unaligned_vector *)(right_frames + tap) * weight;
	}
	*left = (left_sum[0] + left_sum[1]) + (left_sum[2] + left_sum[3]);
	*right = (right_sum[0] + right_sum[1]) + (right_sum[2] + right_sum[3]);
#else
	*left = resampler_dot(left_frames, weights, deltas, blend);
	*right = resampler_dot(right_frames, weights, deltas, blend);
#endif
}

/* a voice starting (over): silence before its first frame, which the output
starts at */
static void resampler_reset(struct sdl_stream *stream)
{
	memset(stream->history, 0, sizeof(stream->history));
	stream->history_count = RESAMPLER_ZERO_CROSSINGS * RESAMPLER_MAXIMUM_STRETCH;
	stream->center = stream->history_count;
	stream->phase = 0.0;
	stream->silence = 0;
}

/* how much the low pass is narrowed for a voice taking frames step at a time:
for one faster than the output rate, to keep what it takes above the output's
band out (up to RESAMPLER_MAXIMUM_STRETCH times) */
static float resampler_scale(double step)
{
	return step > 1.0 ? (float)(1.0 / (step < RESAMPLER_MAXIMUM_STRETCH ? step : RESAMPLER_MAXIMUM_STRETCH)) : 1.0f;
}

static float packet_sample(const struct voice_packet *packet, unsigned long frame, unsigned long channel,
	unsigned long channels)
{
	return packet->samples[frame * channels + channel] * (1.0f / 32768.0f);
}

/* the voice's next frame, finishing the packets it passes; FALSE once they
run out */
static BOOL take_frame(struct sdl_stream *stream, float *frame)
{
	for (;;)
	{
		struct voice_packet *packet = NULL;
		unsigned long position;

		for (position = 0; position < stream->packet_count; position++)
		{
			struct voice_packet *candidate = &stream->packets[(stream->packet_head + position) % MAXIMUM_STREAM_PACKETS];

			if (!candidate->finished)
			{
				packet = candidate;
				break;
			}
		}
		if (!packet)
			return FALSE;
		if (stream->cursor < packet->frames)
		{
			frame[0] = packet_sample(packet, stream->cursor, 0, stream->channels);
			frame[1] = packet_sample(packet, stream->cursor, stream->channels - 1, stream->channels);
			stream->cursor++;
			return TRUE;
		}
		stream->cursor = 0;
		packet->finished = TRUE;
	}
}

/* whether the voice has a frame to take; the packets with none left are
marked finished on the way, as take_frame marks them */
static BOOL voice_has_frames(struct sdl_stream *stream)
{
	unsigned long position;

	for (position = 0; position < stream->packet_count; position++)
	{
		struct voice_packet *packet = &stream->packets[(stream->packet_head + position) % MAXIMUM_STREAM_PACKETS];

		if (packet->finished)
			continue;
		if (stream->cursor < packet->frames)
			return TRUE;
		stream->cursor = 0;
		packet->finished = TRUE;
	}
	return FALSE;
}

/* ---------- mixing */

/* mixes one voice into output (frames of output_channels floats), and into
the reverb's send (frames of mono) */
static void mix_voice(struct sdl_stream *stream, float *output, float *send, unsigned long frames)
{
	double step, target_step, ramp_step;
	float target_left, target_right, target_room, left, right, room, ramp_left, ramp_right, ramp_room, scale;
	float target_direct_lowpass, target_room_lowpass, direct_lowpass, room_lowpass;
	float ramp_direct_lowpass, ramp_room_lowpass;
	float direct_state[2], room_state;
	long width;
	unsigned long frame;
	/* (a stereo voice panned towards its sound: voice_gains) */
	BOOL positioned = stream->channels == 2 && stream->stereo_positioned;
	/* (through the HRTF: its parameters, now and a frame's step of their
	ramp, and its filters' states, here as the low passes' are) */
	BOOL hrtf_on;
	struct hrtf_parameters target_hrtf, hrtf_now, hrtf_ramp;
	float hrtf_rear, hrtf_notch[4], hrtf_shadow[2][2];
	/* on 5.1, the speakers' gains (voice_surround_gains) */
	BOOL surround = output_channels == 6;
	float target_surround[2][MAXIMUM_OUTPUT_CHANNELS], surround_gains[2][MAXIMUM_OUTPUT_CHANNELS];
	float ramp_surround[2][MAXIMUM_OUTPUT_CHANNELS];
	unsigned long speaker;

	if (stream->paused || !stream->packet_count || !stream->sample_rate)
		return;
	target_step = (double)(stream->frequency ? stream->frequency : stream->sample_rate) / OUTPUT_RATE *
		doppler_ratio(stream);
	/* (the step ramps from the last mix's to it, as the gains do) */
	step = stream->gains_valid ? stream->current_step : target_step;
	scale = resampler_scale(step > target_step ? step : target_step);
	width = (long)ceilf(RESAMPLER_ZERO_CROSSINGS / scale);
	/* a voice that ran dry and stopped (below) starts over once it has
	frames again: stream_process starts over only a stream with no packets,
	and the next can come before the finished ones are completed */
	if (stream->silence > (unsigned long)(2 * width))
	{
		if (!voice_has_frames(stream))
			return;
		resampler_reset(stream);
		stream->gains_valid = FALSE;
		step = target_step;
		scale = resampler_scale(step);
		width = (long)ceilf(RESAMPLER_ZERO_CROSSINGS / scale);
	}
	ramp_step = (target_step - step) / (double)frames;
	voice_gains(stream, &target_left, &target_right, &target_room, &target_direct_lowpass, &target_room_lowpass,
		&hrtf_on, &target_hrtf);
	/* (the HRTF is for a pair of ears: none on 5.1) */
	if (surround)
		hrtf_on = FALSE;
	if (hrtf_on && (!stream->gains_valid || !stream->hrtf_on))
	{
		stream->hrtf_current = target_hrtf;
		memset(stream->hrtf_frames, 0, sizeof(stream->hrtf_frames));
		stream->hrtf_rear = 0.0f;
		memset(stream->hrtf_notch, 0, sizeof(stream->hrtf_notch));
		memset(stream->hrtf_shadow, 0, sizeof(stream->hrtf_shadow));
	}
	stream->hrtf_on = hrtf_on;
	if (surround)
		voice_surround_gains(stream, target_surround);
	if (!stream->gains_valid)
	{
		if (surround)
			memcpy(stream->current_surround, target_surround, sizeof(target_surround));
		stream->current_left = target_left;
		stream->current_right = target_right;
		stream->current_room = target_room;
		stream->current_direct_lowpass = target_direct_lowpass;
		stream->current_room_lowpass = target_room_lowpass;
		stream->direct_lowpass[0] = stream->direct_lowpass[1] = 0.0f;
		stream->room_lowpass = 0.0f;
		stream->gains_valid = TRUE;
	}
	left = stream->current_left;
	right = stream->current_right;
	room = stream->current_room;
	direct_lowpass = stream->current_direct_lowpass;
	room_lowpass = stream->current_room_lowpass;
	/* the low passes' states, here while the mix writes the output (which
	the compiler cannot tell from them) */
	direct_state[0] = stream->direct_lowpass[0];
	direct_state[1] = stream->direct_lowpass[1];
	room_state = stream->room_lowpass;
	ramp_left = (target_left - left) / (float)frames;
	ramp_right = (target_right - right) / (float)frames;
	ramp_room = (target_room - room) / (float)frames;
	ramp_direct_lowpass = (target_direct_lowpass - direct_lowpass) / (float)frames;
	ramp_room_lowpass = (target_room_lowpass - room_lowpass) / (float)frames;
	if (hrtf_on)
	{
		int index;

		hrtf_now = stream->hrtf_current;
		for (index = 0; index < 2; index++)
		{
			hrtf_ramp.delay[index] = (target_hrtf.delay[index] - hrtf_now.delay[index]) / (float)frames;
			hrtf_ramp.shadow_b0[index] = (target_hrtf.shadow_b0[index] - hrtf_now.shadow_b0[index]) / (float)frames;
			hrtf_ramp.shadow_b1[index] = (target_hrtf.shadow_b1[index] - hrtf_now.shadow_b1[index]) / (float)frames;
		}
		for (index = 0; index < 5; index++)
			hrtf_ramp.notch[index] = (target_hrtf.notch[index] - hrtf_now.notch[index]) / (float)frames;
		hrtf_ramp.rear = (target_hrtf.rear - hrtf_now.rear) / (float)frames;
		hrtf_rear = stream->hrtf_rear;
		memcpy(hrtf_notch, stream->hrtf_notch, sizeof(hrtf_notch));
		memcpy(hrtf_shadow, stream->hrtf_shadow, sizeof(hrtf_shadow));
	}
	if (surround)
	{
		for (speaker = 0; speaker < MAXIMUM_OUTPUT_CHANNELS; speaker++)
		{
			surround_gains[0][speaker] = stream->current_surround[0][speaker];
			surround_gains[1][speaker] = stream->current_surround[1][speaker];
			ramp_surround[0][speaker] = (target_surround[0][speaker] - surround_gains[0][speaker]) / (float)frames;
			ramp_surround[1][speaker] = (target_surround[1][speaker] - surround_gains[1][speaker]) / (float)frames;
		}
	}

	for (frame = 0; frame < frames; frame++)
	{
		float sample_left = 0.0f, sample_right = 0.0f;
		long tap;

		/* the frames ahead the low pass reaches; after the last packet,
		silence, until what was taken has played out */
		while ((long)(stream->history_count - stream->center) <= width)
		{
			unsigned long slot = stream->history_count % RESAMPLER_HISTORY;
			float taken[2];

			if (take_frame(stream, taken))
			{
				stream->silence = 0;
			}
			else
			{
				taken[0] = taken[1] = 0.0f;
				stream->silence++;
			}
			stream->history[0][slot] = stream->history[0][slot + RESAMPLER_HISTORY] = taken[0];
			stream->history[1][slot] = stream->history[1][slot + RESAMPLER_HISTORY] = taken[1];
			stream->history_count++;
		}
		if (stream->silence > (unsigned long)(2 * width))
			break;

		if (!left && !right && !room && !ramp_left && !ramp_right && !ramp_room)
		{
			/* a voice turned all the way down (out of earshot), and not
			sending to the reverb, only moves on */
		}
		else if (resampler_mode == RESAMPLER_LINEAR)
		{
			/* (the frame the output is at and the next, side by side
			however the ring wraps) */
			const float *left_frames = stream->history[0] + stream->center % RESAMPLER_HISTORY;
			const float *right_frames = stream->history[1] + stream->center % RESAMPLER_HISTORY;
			float fraction = (float)stream->phase;

			sample_left = left_frames[0] + (left_frames[1] - left_frames[0]) * fraction;
			sample_right = right_frames[0] + (right_frames[1] - right_frames[0]) * fraction;
		}
		else if (scale == 1.0f)
		{
			double position = stream->phase * RESAMPLER_TABLE_STEPS;
			unsigned long row = (unsigned long)position;
			float blend = (float)(position - (double)row);
			unsigned long first = (stream->center + 1 - RESAMPLER_ZERO_CROSSINGS) % RESAMPLER_HISTORY;

			/* (a mono voice's two channels are the same: take_frame) */
			if (stream->channels == 1)
			{
				sample_left = sample_right = resampler_dot(stream->history[0] + first, resampler_phases[row],
					resampler_deltas[row], blend);
			}
			else
			{
				resampler_dot2(stream->history[0] + first, stream->history[1] + first, resampler_phases[row],
					resampler_deltas[row], blend, &sample_left, &sample_right);
			}
		}
		else
		{
			unsigned long first = (stream->center + 1 - (unsigned long)width) % RESAMPLER_HISTORY;
			const float *left_frames = stream->history[0] + first, *right_frames = stream->history[1] + first;

			for (tap = 1 - width; tap <= width; tap++)
			{
				float weight = scale * resampler_weight(fabsf((float)tap - (float)stream->phase) * scale * RESAMPLER_TABLE_STEPS);

				sample_left += left_frames[tap + width - 1] * weight;
				if (stream->channels == 2)
					sample_right += right_frames[tap + width - 1] * weight;
			}
			if (stream->channels == 1)
				sample_right = sample_left;
		}
		/* the room send, with its own low pass */
		if (room || ramp_room)
		{
			float mono = stream->channels == 1 ? sample_left : 0.5f * (sample_left + sample_right);

			room_state = flush_denormal(mono + room_lowpass * (room_state - mono));
			send[frame] += room_state * room;
		}
		/* the direct path, muffled, or as it is; a mono voice's mix bins or pan
		split it across the speakers */
		if (direct_lowpass || ramp_direct_lowpass)
		{
			sample_left = flush_denormal(sample_left + direct_lowpass * (direct_state[0] - sample_left));
			sample_right = flush_denormal(sample_right + direct_lowpass * (direct_state[1] - sample_right));
		}
		direct_state[0] = sample_left;
		direct_state[1] = sample_right;
		if (hrtf_on)
		{
			/* through the HRTF: behind, the high frequencies taken off; the
			pinna's notch; then each ear its own delay and the head's shadow */
			float x = sample_left, notched;
			int ear;

			hrtf_rear = flush_denormal(x + hrtf.rear_lowpass * (hrtf_rear - x));
			x -= hrtf_now.rear * (x - hrtf_rear);
			notched = hrtf_now.notch[0] * x + hrtf_now.notch[1] * hrtf_notch[0] + hrtf_now.notch[2] * hrtf_notch[1] -
				hrtf_now.notch[3] * hrtf_notch[2] - hrtf_now.notch[4] * hrtf_notch[3];
			notched = flush_denormal(notched);
			hrtf_notch[1] = hrtf_notch[0];
			hrtf_notch[0] = x;
			hrtf_notch[3] = hrtf_notch[2];
			hrtf_notch[2] = notched;
			stream->hrtf_frames[stream->hrtf_written % HRTF_FRAMES] = notched;
			stream->hrtf_written++;
			for (ear = 0; ear < 2; ear++)
			{
				float heard = hrtf_read(stream->hrtf_frames, stream->hrtf_written, 1.0f + hrtf_now.delay[ear]);
				float shadowed = flush_denormal(hrtf_now.shadow_b0[ear] * heard +
					hrtf_now.shadow_b1[ear] * hrtf_shadow[ear][0] - hrtf.shadow_a1 * hrtf_shadow[ear][1]);

				hrtf_shadow[ear][0] = heard;
				hrtf_shadow[ear][1] = shadowed;
				output[frame * 2 + ear] += shadowed * (ear ? right : left);
			}
			hrtf_step(&hrtf_now, &hrtf_ramp);
		}
		else if (surround)
		{
			/* the first row's gains take the left (a mono voice's only)
			channel, or a positioned voice's middle; the second the right, or
			its difference */
			float *speakers = output + frame * MAXIMUM_OUTPUT_CHANNELS;
			float first = positioned ? 0.5f * (sample_left + sample_right) : sample_left;
			float second = positioned ? 0.5f * (sample_left - sample_right) : stream->channels == 2 ? sample_right : 0.0f;

			for (speaker = 0; speaker < MAXIMUM_OUTPUT_CHANNELS; speaker++)
			{
				speakers[speaker] += first * surround_gains[0][speaker] + second * surround_gains[1][speaker];
				surround_gains[0][speaker] += ramp_surround[0][speaker];
				surround_gains[1][speaker] += ramp_surround[1][speaker];
			}
		}
		else if (stream->channels == 1)
		{
			output[frame * 2] += sample_left * left;
			output[frame * 2 + 1] += sample_left * right;
		}
		else if (positioned)
		{
			/* the channels' middle panned, and their difference kept as
			wide as the far ear's gain: the sound comes from where it is,
			still stereo */
			float middle = 0.5f * (sample_left + sample_right);
			float side = 0.5f * (sample_left - sample_right);
			float far_gain = left < right ? left : right;

			output[frame * 2] += middle * left + side * far_gain;
			output[frame * 2 + 1] += middle * right - side * far_gain;
		}
		else
		{
			output[frame * 2] += sample_left * left;
			output[frame * 2 + 1] += sample_right * right;
		}
		left += ramp_left;
		right += ramp_right;
		room += ramp_room;
		direct_lowpass += ramp_direct_lowpass;
		room_lowpass += ramp_room_lowpass;
		stream->phase += step;
		step += ramp_step;
		while (stream->phase >= 1.0)
		{
			stream->phase -= 1.0;
			stream->center++;
		}
	}
	stream->direct_lowpass[0] = direct_state[0];
	stream->direct_lowpass[1] = direct_state[1];
	stream->room_lowpass = room_state;
	if (hrtf_on)
	{
		stream->hrtf_current = target_hrtf;
		stream->hrtf_rear = hrtf_rear;
		memcpy(stream->hrtf_notch, hrtf_notch, sizeof(hrtf_notch));
		memcpy(stream->hrtf_shadow, hrtf_shadow, sizeof(hrtf_shadow));
	}
	stream->current_left = target_left;
	stream->current_right = target_right;
	stream->current_room = target_room;
	stream->current_step = target_step;
	stream->current_direct_lowpass = target_direct_lowpass;
	stream->current_room_lowpass = target_room_lowpass;
	if (surround)
		memcpy(stream->current_surround, target_surround, sizeof(target_surround));
}

/* ---------- reverb

The Xbox ran an I3DL2 reverb on its audio DSP (the effects image the game
downloads), fed by every 3D voice's I3DL2 mix bin. The game sets the listener
properties from the sound environment the camera is in (sound_dsound_xbox.c
dsound_set_listener_properties), and each 3D voice's room send from its
reverb attenuation and occlusion (dsound_channel_set_I3DL2_properties). This
is a reverb of the I3DL2 model on those properties, not the DSP's program:
	- each 3D voice sends to a mono bus at the listener's and its own room
	  levels, occlusion included, their high frequency levels a low pass at the
	  HF reference, attenuated with distance by the room rolloff factors
	  (voice_gains);
	- the early reflections are REVERB_TAPS taps of the bus, starting the
	  reflections delay later, at the reflections level;
	- the late reverberation starts the reverb delay after the reflections:
	  all-pass diffusers as strong as the diffusion, then a feedback delay
	  network of REVERB_LINES lines, as long as the density makes them, each
	  losing what decays it by 60 dB in the decay time, and its high
	  frequencies in the decay time times the HF ratio, at the reverb level.
Both are normalized to the energy sent, so their levels are relative to the
room level, as I3DL2 gives them. A change of environment crossfades each
delay's read from its old length to its new one, and each gain, over
REVERB_FADE_FRAMES: what is reverberating carries on, without the click of a
jump or the pitch shift of a sliding delay. audio.reverb = false fades the
reverb out over as long and stops it. Once nothing has been sent for
REVERB_QUIET_FRAMES and the reverberation is 120 dB down, the reverb stops
until something is sent again (outdoors, in the menus). */

#define REVERB_DELAY_SIZE 32768 /* the reflections and reverb delays, 0.3 + 0.1 s at most, and the taps */
#define REVERB_TAPS 6
#define REVERB_DIFFUSERS 4
#define REVERB_DIFFUSER_SIZE 1024
#define REVERB_LINES 8
#define REVERB_LINE_SIZE 4096
#define REVERB_FADE_FRAMES 4096 /* 85 ms */
/* the input delays, then the diffusers, played out */
#define REVERB_QUIET_FRAMES (REVERB_DELAY_SIZE + REVERB_DIFFUSERS * REVERB_DIFFUSER_SIZE)
#define REVERB_QUIET_PEAK 1.0e-6f

/* in milliseconds: the taps after the reflections delay, the diffusers, and
the lines at full density */
static const float reverb_tap_times[REVERB_TAPS] = { 0.0f, 3.1f, 5.3f, 7.9f, 11.2f, 14.9f };
static const float reverb_tap_signs[REVERB_TAPS] = { 1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f };
static const float reverb_diffuser_times[REVERB_DIFFUSERS] = { 4.7f, 3.6f, 12.7f, 9.3f };
static const float reverb_line_times[REVERB_LINES] = { 29.7f, 37.1f, 41.1f, 43.7f, 53.3f, 59.9f, 67.1f, 73.1f };
/* which lines go in negated, and the two outputs' signs of the lines */
static const float reverb_input_signs[REVERB_LINES] = { 1, -1, -1, 1, 1, -1, -1, 1 };
static const float reverb_left_signs[REVERB_LINES] = { 1, -1, 1, -1, 1, -1, 1, -1 };
static const float reverb_right_signs[REVERB_LINES] = { 1, 1, -1, -1, 1, 1, -1, -1 };

/* what an environment sets: delays in samples, and gains */
struct reverb_parameters
{
	unsigned long taps[REVERB_TAPS];
	unsigned long late_delay;
	unsigned long lengths[REVERB_LINES];
	/* each line's gain a pass, and its low pass's coefficient */
	float feedback[REVERB_LINES];
	float damping[REVERB_LINES];
	float reflections_gain, late_gain;
	float diffusion;
};

static struct
{
	/* the bus of the current mix */
	float send[MIX_CHUNK_FRAMES];
	/* the environment serial and properties of the parameters faded to */
	unsigned long serial;
	DSI3DL2LISTENER properties;
	/* the parameters fading from and to, and the frames of the fade left
	(none: from is to) */
	struct reverb_parameters from, to;
	unsigned long fade;
	/* the output's level, 1 while audio.reverb is on, moving to 0 when it
	is turned off; whether the buffers are silent; and the frames since the
	send or the output was last over REVERB_QUIET_PEAK */
	float level;
	BOOL silent;
	unsigned long quiet;

	/* the bus, delayed for the taps and the late reverberation */
	float delay[REVERB_DELAY_SIZE];
	unsigned long delay_position;

	float diffusers[REVERB_DIFFUSERS][REVERB_DIFFUSER_SIZE];
	unsigned long diffuser_lengths[REVERB_DIFFUSERS];
	unsigned long diffuser_position;

	float lines[REVERB_LINES][REVERB_LINE_SIZE];
	unsigned long line_position;
	/* each line's low pass */
	float lowpass[REVERB_LINES];
} reverb;

static float clamp_real(float value, float minimum, float maximum)
{
	return value < minimum ? minimum : value > maximum ? maximum : value;
}

/* whole samples */
static unsigned long reverb_samples(float seconds)
{
	return (unsigned long)floorf(seconds * OUTPUT_RATE + 0.5f);
}

/* the sample buffer got delay samples before position, crossfaded from the
from delay to the to delay by fade (0 to 1) */
static float reverb_read(const float *buffer, unsigned long mask, unsigned long position,
	unsigned long from_delay, unsigned long to_delay, float fade)
{
	float from_sample = buffer[(position - from_delay) & mask];

	if (from_delay == to_delay)
		return from_sample;
	return from_sample + (buffer[(position - to_delay) & mask] - from_sample) * fade;
}

static float reverb_blend(float from, float to, float fade)
{
	return from + (to - from) * fade;
}

static void reverb_parameters_set(struct reverb_parameters *parameters, const DSI3DL2LISTENER *properties)
{
	float decay = clamp_real(properties->flDecayTime, 0.1f, 20.0f);
	/* a low pass loses high frequencies; a ratio over 1 would need them to
	last longer */
	float high_decay = decay * clamp_real(properties->flDecayHFRatio, 0.1f, 1.0f);
	float scale = 0.25f + 0.75f * clamp_real(properties->flDensity / 100.0f, 0.0f, 1.0f);
	float cosine = frequency_cosine(properties->flHFReference);
	float mean_length = 0.0f, mean_feedback;
	unsigned long reflections_delay;
	int index;

	for (index = 0; index < REVERB_LINES; index++)
	{
		float length = floorf(reverb_line_times[index] * 0.001f * OUTPUT_RATE * scale + 0.5f);
		float high_feedback;

		parameters->lengths[index] = (unsigned long)length;
		parameters->feedback[index] = powf(10.0f, -3.0f * length / (decay * OUTPUT_RATE));
		high_feedback = powf(10.0f, -3.0f * length / (high_decay * OUTPUT_RATE));
		parameters->damping[index] = lowpass_coefficient(high_feedback / parameters->feedback[index], cosine);
		mean_length += length / REVERB_LINES;
	}
	mean_feedback = powf(10.0f, -3.0f * mean_length / (decay * OUTPUT_RATE));
	reflections_delay = reverb_samples(clamp_real(properties->flReflectionsDelay, 0.0f, 0.3f));
	for (index = 0; index < REVERB_TAPS; index++)
		parameters->taps[index] = reflections_delay + reverb_samples(reverb_tap_times[index] * 0.001f);
	parameters->late_delay = reflections_delay + reverb_samples(clamp_real(properties->flReverbDelay, 0.0f, 0.1f));
	/* the taps' energy, half of them to each side; the network's, whose
	sound circulates until it decays */
	parameters->reflections_gain = gain_from_millibels(properties->lReflections) /
		sqrtf((float)REVERB_TAPS / OUTPUT_CHANNELS);
	parameters->late_gain = gain_from_millibels(properties->lReverb) * sqrtf(1.0f - mean_feedback * mean_feedback);
	parameters->diffusion = 0.7f * clamp_real(properties->flDiffusion / 100.0f, 0.0f, 1.0f);
}

/* a new environment, faded to unless the reverb is silent. The game sets the
same one again several times a second in some places (Derelict), which
changes nothing */
static void reverb_update(const DSI3DL2LISTENER *properties, unsigned long serial)
{
	reverb.serial = serial;
	if (!reverb.silent && !memcmp(properties, &reverb.properties, sizeof(*properties)))
		return;
	reverb.properties = *properties;
	reverb_parameters_set(&reverb.to, properties);
	if (reverb.silent)
		reverb.from = reverb.to;
	reverb.fade = reverb.silent ? 0 : REVERB_FADE_FRAMES;
}

static void reverb_clear(void)
{
	memset(reverb.delay, 0, sizeof(reverb.delay));
	memset(reverb.diffusers, 0, sizeof(reverb.diffusers));
	memset(reverb.lines, 0, sizeof(reverb.lines));
	memset(reverb.lowpass, 0, sizeof(reverb.lowpass));
	reverb.from = reverb.to;
	reverb.fade = 0;
	reverb.silent = TRUE;
	reverb.quiet = 0;
}

static void reverb_initialize(void)
{
	int index;

	for (index = 0; index < REVERB_DIFFUSERS; index++)
		reverb.diffuser_lengths[index] = reverb_samples(reverb_diffuser_times[index] * 0.001f);
	reverb.silent = TRUE;
	reverb.level = reverb_enabled ? 1.0f : 0.0f;
	reverb_update(&environment, environment_serial);
}

/* adds the reverberation of send to output, its level moving to
target_level */
static void reverb_process(const float *send, float *output, unsigned long frames, float target_level)
{
	const struct reverb_parameters *from = &reverb.from, *to = &reverb.to;
	float input_scale = 1.0f / sqrtf((float)REVERB_LINES), peak = 0.0f;
	unsigned long frame;
	int index;

	reverb.silent = FALSE;
	for (frame = 0; frame < frames; frame++)
	{
		/* how far the fade from the old environment to the new is */
		float fade = 1.0f - (float)reverb.fade / REVERB_FADE_FRAMES;
		float early[OUTPUT_CHANNELS] = { 0.0f, 0.0f };
		float outputs[REVERB_LINES], late_left = 0.0f, late_right = 0.0f, feedback_sum = 0.0f, value;
		float diffusion = reverb_blend(from->diffusion, to->diffusion, fade);

		reverb.delay[reverb.delay_position & (REVERB_DELAY_SIZE - 1)] = send[frame];
		for (index = 0; index < REVERB_TAPS; index++)
		{
			early[index % OUTPUT_CHANNELS] += reverb_tap_signs[index] * reverb_read(reverb.delay, REVERB_DELAY_SIZE - 1,
				reverb.delay_position, from->taps[index], to->taps[index], fade);
		}

		/* diffusers: w = x + g w', y = w' - g w */
		value = reverb_read(reverb.delay, REVERB_DELAY_SIZE - 1, reverb.delay_position,
			from->late_delay, to->late_delay, fade);
		reverb.delay_position++;
		for (index = 0; index < REVERB_DIFFUSERS; index++)
		{
			float *buffer = reverb.diffusers[index];
			float delayed = buffer[(reverb.diffuser_position - reverb.diffuser_lengths[index]) & (REVERB_DIFFUSER_SIZE - 1)];
			float written = flush_denormal(value + diffusion * delayed);

			buffer[reverb.diffuser_position & (REVERB_DIFFUSER_SIZE - 1)] = written;
			value = delayed - diffusion * written;
		}
		reverb.diffuser_position++;

		/* the network: each line's output is damped and heard, then decayed
		and mixed back into every line by a Householder reflection */
		for (index = 0; index < REVERB_LINES; index++)
		{
			float line_output = reverb_read(reverb.lines[index], REVERB_LINE_SIZE - 1, reverb.line_position,
				from->lengths[index], to->lengths[index], fade);
			float damping = reverb_blend(from->damping[index], to->damping[index], fade);

			reverb.lowpass[index] = flush_denormal(line_output + damping * (reverb.lowpass[index] - line_output));
			late_left += reverb_left_signs[index] * reverb.lowpass[index];
			late_right += reverb_right_signs[index] * reverb.lowpass[index];
			outputs[index] = reverb.lowpass[index] * reverb_blend(from->feedback[index], to->feedback[index], fade);
			feedback_sum += outputs[index];
		}
		feedback_sum *= 2.0f / REVERB_LINES;
		for (index = 0; index < REVERB_LINES; index++)
		{
			reverb.lines[index][reverb.line_position & (REVERB_LINE_SIZE - 1)] =
				flush_denormal(outputs[index] - feedback_sum + value * reverb_input_signs[index] * input_scale);
		}
		reverb.line_position++;

		{
			float reflections_gain = reverb_blend(from->reflections_gain, to->reflections_gain, fade);
			float late_gain = reverb_blend(from->late_gain, to->late_gain, fade);
			float left = early[0] * reflections_gain + late_left * late_gain;
			float right = early[1] * reflections_gain + late_right * late_gain;

			if (output_channels == 6)
			{
				/* from the front and back pairs, at the same power */
				output[frame * 6] += left * reverb.level * 0.70710678f;
				output[frame * 6 + 1] += right * reverb.level * 0.70710678f;
				output[frame * 6 + 4] += left * reverb.level * 0.70710678f;
				output[frame * 6 + 5] += right * reverb.level * 0.70710678f;
			}
			else
			{
				output[frame * 2] += left * reverb.level;
				output[frame * 2 + 1] += right * reverb.level;
			}
			peak = fabsf(left) > peak ? fabsf(left) : peak;
			peak = fabsf(right) > peak ? fabsf(right) : peak;
			peak = fabsf(send[frame]) > peak ? fabsf(send[frame]) : peak;
		}
		if (reverb.fade && !--reverb.fade)
			reverb.from = reverb.to;
		reverb.level = clamp_real(target_level, reverb.level - 1.0f / REVERB_FADE_FRAMES,
			reverb.level + 1.0f / REVERB_FADE_FRAMES);
	}
	reverb.quiet = peak > REVERB_QUIET_PEAK ? 0 : reverb.quiet + frames;
}

/* whether anything is sent to the reverb in send */
static BOOL reverb_sent(const float *send, unsigned long frames)
{
	unsigned long frame;

	for (frame = 0; frame < frames; frame++)
	{
		if (fabsf(send[frame]) > REVERB_QUIET_PEAK)
			return TRUE;
	}
	return FALSE;
}

/* ---------- limiter

The game sets its mix bins' headroom to 0 (sound_dsound_xbox.c), so the voices
sum at their full level, as on the Xbox, and a pile of loud ones goes over
full scale. Clipping each sample, or bending it near full scale, distorts the
sound: dialogue over gunfire crackled. Instead the whole mix is turned down
for as long as it would go over, both channels alike. The output is delayed
LIMITER_LOOKAHEAD - 1 frames (1.3 ms), so that the gain comes down smoothly
before each peak: the smallest gain the frames ahead need, averaged over the
last LIMITER_LOOKAHEAD frames, is never more than a peak needs when it plays.
The gain comes back up over LIMITER_RELEASE_SECONDS. The ceiling is full
scale: the menus' music is mastered to it (the title theme's samples reach 0
dBFS), and a ceiling under it (it was -1 dBFS) turned the theme down by up to
1.2 dB at each of its loud notes and for the release after, a sixth of its
loud part, where the Xbox played it as it is. Only a sum over full scale is
turned down. */

#define LIMITER_CEILING 1.0f /* 0 dBFS */
#define LIMITER_LOOKAHEAD 64
#define LIMITER_RELEASE_SECONDS 0.1f

static struct
{
	/* the frames the output is delayed by, and the gain each needs */
	float delay[LIMITER_LOOKAHEAD][MAXIMUM_OUTPUT_CHANNELS];
	float needed[LIMITER_LOOKAHEAD];
	/* how many of them are under 1: none while nothing is too loud, when the
	smallest is 1 without looking */
	unsigned long limiting;
	/* the gain held down to what the frames ahead need, coming back up */
	float held;
	/* its last LIMITER_LOOKAHEAD values, and their sum */
	float history[LIMITER_LOOKAHEAD];
	double history_sum;
	unsigned long position;
	BOOL initialized;
} limiter;

static void limit(float *output, unsigned long frames)
{
	float release = 1.0f - expf(-1.0f / (LIMITER_RELEASE_SECONDS * OUTPUT_RATE));
	unsigned long frame, index, channel;

	if (!limiter.initialized)
	{
		for (index = 0; index < LIMITER_LOOKAHEAD; index++)
		{
			limiter.needed[index] = 1.0f;
			limiter.history[index] = 1.0f;
		}
		limiter.held = 1.0f;
		limiter.history_sum = LIMITER_LOOKAHEAD;
		limiter.initialized = TRUE;
	}
	for (frame = 0; frame < frames; frame++)
	{
		float *sample = output + frame * output_channels;
		unsigned long position = limiter.position;
		unsigned long oldest = (position + 1) % LIMITER_LOOKAHEAD;
		float peak = 0.0f, lowest, gain;

		/* the new frame takes the slot of the oldest, which has played */
		for (channel = 0; channel < output_channels; channel++)
		{
			if (fabsf(sample[channel]) > peak)
				peak = fabsf(sample[channel]);
			limiter.delay[position][channel] = sample[channel];
		}
		limiter.limiting -= limiter.needed[position] < 1.0f;
		limiter.needed[position] = peak > LIMITER_CEILING ? LIMITER_CEILING / peak : 1.0f;
		limiter.limiting += limiter.needed[position] < 1.0f;
		lowest = 1.0f;
		if (limiter.limiting)
		{
			lowest = limiter.needed[0];
			for (index = 1; index < LIMITER_LOOKAHEAD; index++)
			{
				if (limiter.needed[index] < lowest)
					lowest = limiter.needed[index];
			}
		}
		if (lowest < limiter.held)
			limiter.held = lowest;
		else
			limiter.held += (lowest - limiter.held) * release;
		limiter.history_sum += limiter.held - limiter.history[position];
		limiter.history[position] = limiter.held;
		gain = (float)(limiter.history_sum / LIMITER_LOOKAHEAD);
		/* the frame LIMITER_LOOKAHEAD - 1 frames old plays */
		for (channel = 0; channel < output_channels; channel++)
			sample[channel] = limiter.delay[oldest][channel] * gain;
		limiter.position = oldest;
	}
}

/* ---------- recording

debug.audio_record: the mix as it plays, to a WAV file of 16-bit stereo at
the output rate, its sizes written again each second, so that a test that
stops the game mid-run still leaves a whole file. On the mixer's thread (the
device's or the clock's); opened before either starts (audio_start). */

static FILE *record_file;
static unsigned long record_frames;

static void record_sizes(void)
{
	unsigned long bytes = record_frames * OUTPUT_CHANNELS * 2;
	unsigned char size[4];
	long end = ftell(record_file);

	size[0] = (unsigned char)(bytes + 36);
	size[1] = (unsigned char)((bytes + 36) >> 8);
	size[2] = (unsigned char)((bytes + 36) >> 16);
	size[3] = (unsigned char)((bytes + 36) >> 24);
	fseek(record_file, 4, SEEK_SET);
	fwrite(size, 1, 4, record_file);
	size[0] = (unsigned char)bytes;
	size[1] = (unsigned char)(bytes >> 8);
	size[2] = (unsigned char)(bytes >> 16);
	size[3] = (unsigned char)(bytes >> 24);
	fseek(record_file, 40, SEEK_SET);
	fwrite(size, 1, 4, record_file);
	fseek(record_file, end, SEEK_SET);
	fflush(record_file);
}

static void record_open(void)
{
	static const unsigned char header[44] =
	{
		'R', 'I', 'F', 'F', 36, 0, 0, 0, 'W', 'A', 'V', 'E',
		'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, OUTPUT_CHANNELS, 0,
		OUTPUT_RATE & 0xff, (OUTPUT_RATE >> 8) & 0xff, (OUTPUT_RATE >> 16) & 0xff, 0,
		(OUTPUT_RATE * OUTPUT_CHANNELS * 2) & 0xff, ((OUTPUT_RATE * OUTPUT_CHANNELS * 2) >> 8) & 0xff,
		((OUTPUT_RATE * OUTPUT_CHANNELS * 2) >> 16) & 0xff, 0,
		OUTPUT_CHANNELS * 2, 0, 16, 0,
		'd', 'a', 't', 'a', 0, 0, 0, 0,
	};
	const char *path = config_string("debug.audio_record");

	if (!path || !path[0])
		return;
	record_file = fopen(path, "wb");
	if (!record_file)
	{
		platform_log("audio: cannot record to %s", path);
		return;
	}
	fwrite(header, 1, sizeof(header), record_file);
	fflush(record_file);
	platform_log("audio: recording to %s", path);
}

static void record(const float *output, unsigned long frames)
{
	short samples[MIX_CHUNK_FRAMES * OUTPUT_CHANNELS];
	unsigned long index, count = frames * OUTPUT_CHANNELS;

	if (!record_file)
		return;
	if (count > MIX_CHUNK_FRAMES * OUTPUT_CHANNELS)
		count = MIX_CHUNK_FRAMES * OUTPUT_CHANNELS;
	for (index = 0; index < count; index++)
	{
		/* (on 5.1, the front pair) rounded to the nearest step, at the
		scale the mixer reads samples at, so that a voice played as it is is
		recorded as it is; cutting toward zero took half a step off each
		sample on average */
		float sample = output[index / OUTPUT_CHANNELS * output_channels + index % OUTPUT_CHANNELS] * 32768.0f;

		samples[index] = (short)lrintf(sample > 32767.0f ? 32767.0f : (sample < -32768.0f ? -32768.0f : sample));
	}
	fwrite(samples, sizeof(short), count, record_file);
	if ((record_frames + frames) / OUTPUT_RATE != record_frames / OUTPUT_RATE)
	{
		record_frames += frames;
		record_sizes();
	}
	else
	{
		record_frames += frames;
	}
}

/* debug.audio_log: the time the mixes took (nanoseconds) and the frames
they mixed, since the log last said */
static Uint64 mix_nanoseconds;
static unsigned long mix_frames;

static void mix(float *output, unsigned long frames)
{
	struct sdl_stream *stream;
	DSI3DL2LISTENER properties;
	BOOL enabled = reverb_enabled, changed = FALSE;
	unsigned long serial = 0;
	Uint64 started = SDL_GetTicksNS();

	memset(output, 0, frames * output_channels * sizeof(float));
	memset(reverb.send, 0, frames * sizeof(float));
	pthread_mutex_lock(&mixer_lock);
	for (stream = streams; stream; stream = stream->next)
		mix_voice(stream, output, reverb.send, frames);
	/* a new environment, once the fade to the last is over */
	if (reverb.serial != environment_serial && !reverb.fade)
	{
		properties = environment;
		serial = environment_serial;
		changed = TRUE;
	}
	pthread_mutex_unlock(&mixer_lock);
	if (changed)
		reverb_update(&properties, serial);
	/* the reverb runs while something reverberates, and stops once it is
	turned off and faded out, or quiet */
	if (!reverb.silent || (enabled && reverb_sent(reverb.send, frames)))
	{
		reverb_process(reverb.send, output, frames, enabled ? 1.0f : 0.0f);
		if ((!enabled && reverb.level <= 0.0f) || reverb.quiet > REVERB_QUIET_FRAMES)
			reverb_clear();
	}
	/* the players' voices (voice_audio.c), dry, under the limiter; on 5.1
	from the front pair */
	if (output_channels == 6)
	{
		float voices[MIX_CHUNK_FRAMES * OUTPUT_CHANNELS];
		unsigned long frame;

		memset(voices, 0, frames * OUTPUT_CHANNELS * sizeof(float));
		voice_audio_mix(voices, frames);
		for (frame = 0; frame < frames; frame++)
		{
			output[frame * 6] += voices[frame * 2];
			output[frame * 6 + 1] += voices[frame * 2 + 1];
		}
	}
	else
	{
		voice_audio_mix(output, frames);
	}
	limit(output, frames);
	record(output, frames);
	mix_nanoseconds += SDL_GetTicksNS() - started;
	mix_frames += frames;
}

/* ---------- the output's channels

audio.channels (setting: "stereo", "5.1" or "auto"; anything else is
stereo) and the channels of the device the sound plays on (device_channels;
0 when it cannot say): 6 for 5.1, 2 for stereo. "auto" is 5.1 on a device
of six channels or more (a 5.1 or 7.1 receiver, a surround headset: SDL
lays the 5.1 out on a 7.1's speakers), stereo on fewer (speakers, a pair
of headphones) and when it cannot say. On 5.1 the speakers are panned
(surround_pan), never the HRTF, which is for a pair of ears. */
static unsigned long output_channels_choose(const char *setting, int device_channels)
{
	if (setting && !strcmp(setting, "5.1"))
		return 6;
	if (setting && !strcmp(setting, "auto") && device_channels >= 6)
		return 6;
	return 2;
}

/* ---------- output */

static SDL_AudioStream *audio_stream;
static BOOL audio_started = FALSE;
/* the device it plays on (audio.output_device), and when it was looked at */
static char audio_device_name[PLATFORM_AUDIO_DEVICE_NAME_SIZE];
static unsigned long audio_device_read_at = (unsigned long)-1;

/* audio.output_device ("default": the system's; none on Android) */
static const char *audio_device_setting(void)
{
	const char *name = config_string("audio.output_device");

	return name && name[0] ? name : "default";
}

static void SDLCALL audio_callback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount)
{
	float buffer[MIX_CHUNK_FRAMES * MAXIMUM_OUTPUT_CHANNELS];

	(void)userdata;
	(void)total_amount;
	while (additional_amount > 0)
	{
		unsigned long frames = (unsigned long)additional_amount / (output_channels * sizeof(float));

		if (frames > MIX_CHUNK_FRAMES)
			frames = MIX_CHUNK_FRAMES;
		if (!frames)
			frames = 1;
		mix(buffer, frames);
		SDL_PutAudioStreamData(stream, buffer, (int)(frames * output_channels * sizeof(float)));
		additional_amount -= (int)(frames * output_channels * sizeof(float));
	}
}

/* without a device, drain voices in real time */
/* debug.audio_record with debug.frame_time (footage: the frames drawn at
a fixed time each, slower or faster than the clock): the mix paced by the
frames instead, each drawn frame owing its time's sound, which the frame
waits to be mixed (dsound_sdl_frame), so that the recording keeps step with
the pictures */
static BOOL paced;
static pthread_mutex_t paced_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t paced_condition = PTHREAD_COND_INITIALIZER;
static double paced_owed;

static void *silent_clock_thread(void *parameter)
{
	float buffer[480 * MAXIMUM_OUTPUT_CHANNELS];
	struct timespec next;

	(void)parameter;
	while (paced)
	{
		pthread_mutex_lock(&paced_lock);
		while (paced_owed < 480.0)
			pthread_cond_wait(&paced_condition, &paced_lock);
		pthread_mutex_unlock(&paced_lock);
		mix(buffer, 480);
		pthread_mutex_lock(&paced_lock);
		paced_owed -= 480.0;
		pthread_cond_broadcast(&paced_condition);
		pthread_mutex_unlock(&paced_lock);
	}
	clock_gettime(CLOCK_MONOTONIC, &next);
	for (;;)
	{
		mix(buffer, 480);
		next.tv_nsec += 10000000L;
		if (next.tv_nsec >= 1000000000L)
		{
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
	}
	return NULL;
}

/* (the presenting thread, each frame drawn) its time's sound owed to a
paced mix, waited for */
void dsound_sdl_frame(double seconds)
{
	if (!paced)
		return;
	pthread_mutex_lock(&paced_lock);
	paced_owed += seconds * OUTPUT_RATE;
	pthread_cond_broadcast(&paced_condition);
	while (paced_owed >= 480.0)
		pthread_cond_wait(&paced_condition, &paced_lock);
	pthread_mutex_unlock(&paced_lock);
}

/* the frames of sound recorded so far (debug.audio_record) */
unsigned long dsound_sdl_recorded_frames(void)
{
	return record_frames;
}

/* audio.channels on Android: stereo; elsewhere what output_channels_choose
makes of the setting and of the channels the device has, which the host's
SDL says (host_desktop.c: SDL_GetAudioDeviceFormat, the format the device
plays in, not one SDL would convert to) */
static unsigned long choose_output_channels(SDL_AudioDeviceID device)
{
#ifdef HALO_ANDROID
	(void)device;
	return 2;
#else
	const char *setting = config_string("audio.channels");
	SDL_AudioSpec spec;
	int device_channels = 0;

	if (setting && !strcmp(setting, "auto") && SDL_GetAudioDeviceFormat(device, &spec, NULL))
		device_channels = spec.channels;
	if (setting && strcmp(setting, "auto") && strcmp(setting, "stereo") && strcmp(setting, "5.1"))
		platform_log("audio.channels \"%s\" is not \"auto\", \"stereo\" or \"5.1\"; using stereo", setting);
	if (setting && !strcmp(setting, "auto"))
	{
		platform_log("sound: the device has %d channels%s", device_channels,
			device_channels ? "" : " (it cannot say: stereo)");
	}
	return output_channels_choose(setting, device_channels);
#endif
}

static void audio_start(void)
{
	SDL_AudioSpec spec;

	if (audio_started)
		return;
	audio_started = TRUE;
	master_volume = (float)config_real("audio.volume");
	reverb_enabled = config_boolean("audio.reverb");
	doppler_enabled = config_boolean("audio.doppler");
	resampler_mode = resampler_mode_read();
	resampler_initialize();
	resampler_phases_initialize();
	reverb_initialize();
	hrtf_initialize();
	record_open();
	paced = record_file && config_real("debug.frame_time") > 0.0;
	if (paced)
		platform_log("sound: recorded in step with the frames (debug.frame_time), no device");

	if (!paced && config_boolean("audio.enabled") && platform_sdl_initialize())
	{
		snprintf(audio_device_name, sizeof(audio_device_name), "%s", audio_device_setting());
		output_channels = choose_output_channels(platform_audio_device(FALSE, audio_device_name));
		platform_log("sound: %s", output_channels == 6 ? "5.1 surround" : "stereo");
		spec.format = SDL_AUDIO_F32;
		spec.channels = (int)output_channels;
		spec.freq = OUTPUT_RATE;
		{
			/* frames per callback, which the host's SDL takes (host_sdl.c's
			hint): each callback is handed to a thread that can run the
			guest, and on Android 512 left it too little time and the menus'
			music broke up, which 1024 does not (about 21 ms at 48 kHz, 11
			ms more than 512). The desktop's: audio.buffer_frames
			(port_config.c), 1024 unless set */
#ifdef HALO_ANDROID
			long buffer_frames = 1024;
#else
			long buffer_frames = config_integer("audio.buffer_frames");
#endif
			char frames[16];

			if (buffer_frames < 64)
				buffer_frames = 64;
			if (buffer_frames > 8192)
				buffer_frames = 8192;
			snprintf(frames, sizeof(frames), "%ld", buffer_frames);
			SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames);
		}
		audio_device_read_at = config_changes();
		audio_stream = SDL_OpenAudioDeviceStream(platform_audio_device(FALSE, audio_device_name), &spec,
			audio_callback, NULL);
		if (!audio_stream && strcmp(audio_device_name, "default"))
			audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_callback, NULL);
		if (audio_stream)
		{
			SDL_ResumeAudioStreamDevice(audio_stream);
			return;
		}
		platform_log("cannot open an audio device (%s); sound is silent", SDL_GetError());
		output_channels = 2;
	}
	{
		pthread_t thread;

		pthread_create(&thread, NULL, silent_clock_thread, NULL);
		pthread_detach(thread);
	}
}

/* (the event thread, each frame: sdl_platform.c) audio.output_device
changed (Settings > Audio): the sound goes on on the new device, else the
system's default */
void dsound_sdl_output_device_check(void)
{
	SDL_AudioSpec spec;
	SDL_AudioStream *stream;

	if (!audio_stream || audio_device_read_at == config_changes())
		return;
	audio_device_read_at = config_changes();
	if (!strcmp(audio_device_name, audio_device_setting()))
		return;
	snprintf(audio_device_name, sizeof(audio_device_name), "%s", audio_device_setting());
	spec.format = SDL_AUDIO_F32;
	/* (the game's mix bins are set for the channels the sound started with;
	SDL turns them into the new device's) */
	spec.channels = (int)output_channels;
	spec.freq = OUTPUT_RATE;
	SDL_DestroyAudioStream(audio_stream);
	stream = SDL_OpenAudioDeviceStream(platform_audio_device(FALSE, audio_device_name), &spec, audio_callback, NULL);
	if (!stream)
		stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_callback, NULL);
	audio_stream = stream;
	if (audio_stream)
	{
		SDL_ResumeAudioStreamDevice(audio_stream);
		platform_log("audio: playing on %s", audio_device_name);
	}
	else
	{
		pthread_t thread;

		/* (none at all: the voices drained in real time, as at the start) */
		platform_log("audio: cannot open an audio device (%s); sound is silent", SDL_GetError());
		pthread_create(&thread, NULL, silent_clock_thread, NULL);
		pthread_detach(thread);
	}
}

/* ---------- completion */

static void packet_release(struct voice_packet *entry)
{
	free(entry->samples);
	entry->samples = NULL;
}

/* completes the head packet; called with the lock held, which the game's
callback runs without */
static void stream_complete_head(struct sdl_stream *stream, DWORD status, DWORD completed_size)
{
	struct voice_packet *entry = &stream->packets[stream->packet_head];
	XMEDIAPACKET packet = entry->packet;

	/* (one not played to its end, flushed: the cursor was its place, and the
	mixer may take the next while the lock is let go below) */
	if (!entry->finished)
		stream->cursor = 0;
	packet_release(entry);
	entry->finished = FALSE;
	stream->packet_head = (stream->packet_head + 1) % MAXIMUM_STREAM_PACKETS;
	stream->packet_count--;
	if (packet.pdwCompletedSize)
		*packet.pdwCompletedSize = completed_size;
	if (packet.pdwStatus)
		*packet.pdwStatus = status;
	if (stream->callback)
	{
		pthread_mutex_unlock(&mixer_lock);
		stream->callback(stream->context, packet.pContext, status);
		pthread_mutex_lock(&mixer_lock);
	}
	else if (packet.hCompletionEvent)
	{
		SetEvent(packet.hCompletionEvent);
	}
}

static void streams_complete_finished(void)
{
	struct sdl_stream *stream;

	pthread_mutex_lock(&mixer_lock);
	for (stream = streams; stream; stream = stream->next)
	{
		while (stream->packet_count && stream->packets[stream->packet_head].finished)
			stream_complete_head(stream, XMEDIAPACKET_STATUS_SUCCESS, stream->packets[stream->packet_head].packet.dwMaxSize);
	}
	pthread_mutex_unlock(&mixer_lock);
}

/* ---------- stream interface */

static struct sdl_stream *stream_from_interface(void *stream)
{
	return (struct sdl_stream *)stream;
}

static ULONG STDMETHODCALLTYPE stream_add_reference(IDirectSoundStream *object)
{
	struct sdl_stream *stream = stream_from_interface(object);
	ULONG count;

	pthread_mutex_lock(&mixer_lock);
	count = ++stream->reference_count;
	pthread_mutex_unlock(&mixer_lock);
	return count;
}

static HRESULT STDMETHODCALLTYPE stream_flush(IDirectSoundStream *object);

static ULONG STDMETHODCALLTYPE stream_release(IDirectSoundStream *object)
{
	struct sdl_stream *stream = stream_from_interface(object);
	struct sdl_stream **link;
	ULONG count;

	pthread_mutex_lock(&mixer_lock);
	count = --stream->reference_count;
	pthread_mutex_unlock(&mixer_lock);
	if (count)
		return count;

	stream_flush(object);
	pthread_mutex_lock(&mixer_lock);
	for (link = &streams; *link; link = &(*link)->next)
	{
		if (*link == stream)
		{
			*link = stream->next;
			break;
		}
	}
	pthread_mutex_unlock(&mixer_lock);
	free(stream);
	return 0;
}

static HRESULT STDMETHODCALLTYPE stream_get_info(IDirectSoundStream *object, LPXMEDIAINFO information)
{
	struct sdl_stream *stream = stream_from_interface(object);

	memset(information, 0, sizeof(*information));
	information->dwFlags = XMO_STREAMF_FIXED_SAMPLE_SIZE | XMO_STREAMF_INPUT_ASYNC;
	information->dwInputSize = stream->adpcm ? XBOX_ADPCM_BLOCK_BYTES * stream->channels : 2 * stream->channels;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE stream_get_status(IDirectSoundStream *object, LPDWORD status)
{
	struct sdl_stream *stream = stream_from_interface(object);

	pthread_mutex_lock(&mixer_lock);
	*status = stream->packet_count < MAXIMUM_STREAM_PACKETS ? XMO_STATUSF_ACCEPT_INPUT_DATA : 0;
	pthread_mutex_unlock(&mixer_lock);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE stream_process(IDirectSoundStream *object, LPCXMEDIAPACKET input, LPCXMEDIAPACKET output)
{
	struct sdl_stream *stream = stream_from_interface(object);
	struct voice_packet *entry;
	unsigned long frames = 0;
	short *samples;

	(void)output;
	if (!input)
		return E_INVALIDARG;
	/* decode outside the lock */
	samples = stream->adpcm ?
		decode_adpcm(input->pvBuffer, input->dwMaxSize, stream->channels, &frames) :
		decode_pcm(input->pvBuffer, input->dwMaxSize, stream->channels, &frames);
	pthread_mutex_lock(&mixer_lock);
	if (stream->packet_count == MAXIMUM_STREAM_PACKETS)
	{
		pthread_mutex_unlock(&mixer_lock);
		free(samples);
		return E_OUTOFMEMORY;
	}
	entry = &stream->packets[(stream->packet_head + stream->packet_count) % MAXIMUM_STREAM_PACKETS];
	entry->packet = *input;
	entry->samples = samples;
	entry->frames = samples ? frames : 0;
	entry->finished = FALSE;
	if (input->pdwStatus)
		*input->pdwStatus = XMEDIAPACKET_STATUS_PENDING;
	if (input->pdwCompletedSize)
		*input->pdwCompletedSize = 0;
	if (!stream->packet_count)
	{
		/* a stream that ran dry starts over */
		stream->cursor = 0;
		resampler_reset(stream);
		stream->gains_valid = FALSE;
	}
	stream->packet_count++;
	pthread_mutex_unlock(&mixer_lock);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE stream_discontinuity(IDirectSoundStream *object)
{
	(void)object;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE stream_flush(IDirectSoundStream *object)
{
	struct sdl_stream *stream = stream_from_interface(object);

	pthread_mutex_lock(&mixer_lock);
	while (stream->packet_count)
	{
		struct voice_packet *head = &stream->packets[stream->packet_head];

		/* Cancellation is synchronous. SUCCESS would let the game's
		completion callback refill the stream we are stopping. */
		stream_complete_head(stream, XMEDIAPACKET_STATUS_FLUSHED,
			head->finished ? head->packet.dwMaxSize : 0);
	}
	stream->cursor = 0;
	resampler_reset(stream);
	/* (the next sound on the channel says where it is) */
	stream->stereo_positioned = FALSE;
	pthread_mutex_unlock(&mixer_lock);
	return S_OK;
}

/* A stereo voice of a sound in the world (`positioned`) is panned towards
it, `pan` from -1 (left) to 1 (right), and muffled and reverberated as a 3D
voice is (its I3DL2 source, which the game sets as a 3D channel's), its room
send rolling off from `minimum_distance` with `distance`. The game fades its
volume with distance itself, by `distance_fade`. Not positioned, it plays as
the Xbox played every stereo sound, unpanned and dry. (sound_manager.c,
update_channels: sound_dsound_xbox.c calls this.) */
void dsound_sdl_stream_set_stereo_position(IDirectSoundStream *object, BOOL positioned, float pan,
	float distance, float minimum_distance, float distance_fade)
{
	struct sdl_stream *stream = stream_from_interface(object);

	pthread_mutex_lock(&mixer_lock);
	stream->stereo_positioned = positioned && stream->channels == 2;
	stream->stereo_pan = pan < -1.0f ? -1.0f : (pan > 1.0f ? 1.0f : pan);
	stream->stereo_distance = distance > 0.0f ? distance : 0.0f;
	stream->stereo_distance_fade = distance_fade < 0.0f ? 0.0f : (distance_fade > 1.0f ? 1.0f : distance_fade);
	/* (as the game gives a 3D channel: no maximum) */
	stream->minimum_distance = minimum_distance > 0.0f ? minimum_distance : 0.0f;
	stream->maximum_distance = 3.4e38f;
	pthread_mutex_unlock(&mixer_lock);
}

static IDirectSoundStreamVtbl stream_vtable =
{
	stream_add_reference,
	stream_release,
	stream_get_info,
	stream_get_status,
	stream_process,
	stream_discontinuity,
	stream_flush,
};

/* ---------- the DirectSound object */

struct sdl_direct_sound
{
	ULONG reference_count;
};

static struct sdl_direct_sound direct_sound = { 0 };

HRESULT WINAPI DirectSoundCreate(LPGUID device_id, LPDIRECTSOUND *result, LPUNKNOWN outer)
{
	(void)device_id;
	(void)outer;
	audio_start();
	direct_sound.reference_count++;
	*result = (LPDIRECTSOUND)&direct_sound;
	return DS_OK;
}

ULONG WINAPI IDirectSound_Release(LPDIRECTSOUND sound)
{
	(void)sound;
	return direct_sound.reference_count ? --direct_sound.reference_count : 0;
}

/* debug.audio_log: each 3D voice playing (a sound placed in the world),
where it is from the listener and its gains, as the mixer has them now */
static void voices_log(void)
{
	static Uint64 logged_at;
	struct sdl_stream *stream;
	Uint64 now = SDL_GetTicks();

	if (now - logged_at < 200)
		return;
	logged_at = now;
	/* (what the mixing costs: its time over the time it mixed; and where
	the recording is) */
	if (mix_frames)
	{
		char recorded[48] = "";

		if (record_file)
			snprintf(recorded, sizeof(recorded), "; %.2f s recorded", (double)record_frames / OUTPUT_RATE);
		platform_log("audio: mixing took %.2f%% of the time it mixed (the HRTF %s)%s", 100.0 *
			(double)mix_nanoseconds / ((double)mix_frames * 1.0e9 / OUTPUT_RATE), hrtf_enabled ? "on" : "off",
			recorded);
		mix_nanoseconds = 0;
		mix_frames = 0;
	}
	pthread_mutex_lock(&mixer_lock);
	for (stream = streams; stream; stream = stream->next)
	{
		float offset[3], right_axis[3], left, right, room, direct_lowpass, room_lowpass;
		BOOL hrtf_on;
		struct hrtf_parameters parameters;
		char heard[160] = "";
		int axis;

		if (!stream->has_3d || stream->mode == DS3DMODE_DISABLE || stream->paused || !stream->packet_count)
			continue;
		for (axis = 0; axis < 3; axis++)
		{
			offset[axis] = stream->position[axis] -
				(stream->mode == DS3DMODE_HEADRELATIVE ? 0.0f : listener.position[axis]);
		}
		right_axis[0] = listener.top[1] * listener.front[2] - listener.top[2] * listener.front[1];
		right_axis[1] = listener.top[2] * listener.front[0] - listener.top[0] * listener.front[2];
		right_axis[2] = listener.top[0] * listener.front[1] - listener.top[1] * listener.front[0];
		voice_gains(stream, &left, &right, &room, &direct_lowpass, &room_lowpass, &hrtf_on, &parameters);
		if (hrtf_on)
		{
			/* (the shadows' gains at the highest frequencies: (b0 - b1) /
			(1 - a1)) */
			snprintf(heard, sizeof(heard), "; HRTF: the right ear %+.0f us after the left, high frequencies left "
				"%+.1f dB, right %+.1f dB, notch %.0f Hz, behind %+.1f dB",
				(parameters.delay[1] - parameters.delay[0]) * 1.0e6f / OUTPUT_RATE,
				20.0f * log10f((parameters.shadow_b0[0] - parameters.shadow_b1[0]) / (1.0f - hrtf.shadow_a1)),
				20.0f * log10f((parameters.shadow_b0[1] - parameters.shadow_b1[1]) / (1.0f - hrtf.shadow_a1)),
				parameters.notch_frequency, 20.0f * log10f(1.0f - parameters.rear * (1.0f - (1.0f - hrtf.rear_lowpass) /
				(1.0f + hrtf.rear_lowpass))));
		}
		platform_log("audio: voice %p: %.2f m ahead, %.2f right, %.2f up (minimum distance %.2f m); gains left %.1f "
			"dB, right %.1f dB; Doppler x%.3f%s", (void *)stream, dot3(offset, listener.front) * listener.distance_factor,
			dot3(offset, right_axis) * listener.distance_factor, dot3(offset, listener.top) * listener.distance_factor,
			stream->minimum_distance * listener.distance_factor,
			left > 0.0f ? 20.0f * log10f(left) : -999.0f, right > 0.0f ? 20.0f * log10f(right) : -999.0f,
			doppler_ratio(stream), heard);
	}
	pthread_mutex_unlock(&mixer_lock);
}

#ifdef HALO_VR
int halo_vr_running(void);
#endif

VOID WINAPI DirectSoundDoWork(void)
{
	static unsigned long volume_read_at = (unsigned long)-1;
	static BOOL log_voices;
#ifdef HALO_VR
	static BOOL hrtf_setting;
#endif

	/* (audio.volume, audio.reverb, audio.doppler and audio.resampler read
	again when the settings change: on the game's thread, not the mixer's,
	whose lock the config's file I/O would hold) */
	if (volume_read_at != config_changes())
	{
		volume_read_at = config_changes();
		master_volume = (float)config_real("audio.volume");
		reverb_enabled = config_boolean("audio.reverb");
		doppler_enabled = config_boolean("audio.doppler");
		log_voices = config_boolean("debug.audio_log");
#ifdef HALO_VR
		hrtf_setting = config_boolean("vr.hrtf");
#endif
		resampler_mode = resampler_mode_read();
	}
#ifdef HALO_VR
	/* (the HRTF in the VR mode alone: on the flat screen, the pan) */
	hrtf_enabled = hrtf_setting && halo_vr_running();
#endif
	if (log_voices)
		voices_log();
	streams_complete_finished();
}

VOID WINAPI DirectSoundUseFullHRTF(void)
{
}

HRESULT WINAPI IDirectSound_GetCaps(LPDIRECTSOUND sound, LPDSCAPS caps)
{
	(void)sound;
	memset(caps, 0, sizeof(*caps));
	caps->dwFree2DBuffers = 64;
	caps->dwFree3DBuffers = 64;
	caps->dwFreeBufferSGEs = 2047;
	caps->dwMemoryAllocated = 0;
	return DS_OK;
}

HRESULT WINAPI IDirectSound_GetSpeakerConfig(LPDIRECTSOUND sound, LPDWORD speaker_config)
{
	(void)sound;
	*speaker_config = output_channels == 6 ? DSSPEAKER_ENABLE_AC3 : DSSPEAKER_STEREO;
	return DS_OK;
}

HRESULT WINAPI IDirectSound_DownloadEffectsImage(LPDIRECTSOUND sound, LPCVOID image, DWORD image_size,
	LPCDSEFFECTIMAGELOC image_location, LPDSEFFECTIMAGEDESC *image_description)
{
	(void)sound;
	(void)image;
	(void)image_size;
	(void)image_location;
	if (image_description)
		*image_description = NULL;
	return DS_OK;
}

HRESULT WINAPI IDirectSound_CommitDeferredSettings(LPDIRECTSOUND sound) { (void)sound; return DS_OK; }
HRESULT WINAPI IDirectSound_SetMixBinHeadroom(LPDIRECTSOUND sound, DWORD mix_bin_mask, DWORD headroom) { (void)sound; (void)mix_bin_mask; (void)headroom; return DS_OK; }
HRESULT WINAPI IDirectSound_SetI3DL2Listener(LPDIRECTSOUND sound, LPCDSI3DL2LISTENER listener_properties, DWORD apply)
{
	(void)sound;
	(void)apply;
	pthread_mutex_lock(&mixer_lock);
	environment = *listener_properties;
	environment_serial++;
	pthread_mutex_unlock(&mixer_lock);
	return DS_OK;
}

HRESULT WINAPI IDirectSound_SetDistanceFactor(LPDIRECTSOUND sound, FLOAT factor, DWORD apply)
{
	(void)sound;
	(void)apply;
	pthread_mutex_lock(&mixer_lock);
	listener.distance_factor = factor > 0.0f ? factor : 1.0f;
	pthread_mutex_unlock(&mixer_lock);
	return DS_OK;
}

HRESULT WINAPI IDirectSound_SetRolloffFactor(LPDIRECTSOUND sound, FLOAT factor, DWORD apply)
{
	(void)sound;
	(void)apply;
	pthread_mutex_lock(&mixer_lock);
	listener.rolloff_factor = factor >= 0.0f ? factor : 1.0f;
	pthread_mutex_unlock(&mixer_lock);
	return DS_OK;
}

HRESULT WINAPI IDirectSound_SetPosition(LPDIRECTSOUND sound, FLOAT x, FLOAT y, FLOAT z, DWORD apply)
{
	(void)sound;
	(void)apply;
	pthread_mutex_lock(&mixer_lock);
	listener.position[0] = x;
	listener.position[1] = y;
	listener.position[2] = z;
	pthread_mutex_unlock(&mixer_lock);
	return DS_OK;
}

HRESULT WINAPI IDirectSound_SetVelocity(LPDIRECTSOUND sound, FLOAT x, FLOAT y, FLOAT z, DWORD apply)
{
	(void)sound;
	(void)apply;
	pthread_mutex_lock(&mixer_lock);
	listener.velocity[0] = x;
	listener.velocity[1] = y;
	listener.velocity[2] = z;
	pthread_mutex_unlock(&mixer_lock);
	return DS_OK;
}

static void normalize3(float *vector)
{
	float length = sqrtf(dot3(vector, vector));

	if (length > 1.0e-6f)
	{
		vector[0] /= length;
		vector[1] /= length;
		vector[2] /= length;
	}
}

HRESULT WINAPI IDirectSound_SetOrientation(LPDIRECTSOUND sound, FLOAT x_front, FLOAT y_front, FLOAT z_front,
	FLOAT x_top, FLOAT y_top, FLOAT z_top, DWORD apply)
{
	(void)sound;
	(void)apply;
	pthread_mutex_lock(&mixer_lock);
	listener.front[0] = x_front;
	listener.front[1] = y_front;
	listener.front[2] = z_front;
	listener.top[0] = x_top;
	listener.top[1] = y_top;
	listener.top[2] = z_top;
	normalize3(listener.front);
	normalize3(listener.top);
	pthread_mutex_unlock(&mixer_lock);
	return DS_OK;
}

HRESULT WINAPI IDirectSound_CreateSoundStream(LPDIRECTSOUND sound, LPCDSSTREAMDESC description,
	LPDIRECTSOUNDSTREAM *result, LPUNKNOWN outer)
{
	struct sdl_stream *stream = calloc(1, sizeof(*stream));
	const WAVEFORMATEX *format = description->lpwfxFormat;

	(void)sound;
	(void)outer;
	if (!stream)
		return E_OUTOFMEMORY;
	stream->object.lpVtbl = &stream_vtable;
	stream->reference_count = 1;
	stream->callback = description->lpfnCallback;
	stream->context = description->lpvContext;
	stream->adpcm = format && format->wFormatTag == WAVE_FORMAT_XBOX_ADPCM;
	stream->channels = format && format->nChannels == 2 ? 2 : 1;
	stream->sample_rate = format ? format->nSamplesPerSec : 0;
	stream->frequency = stream->sample_rate;
	stream->volume = 1.0f;
	/* DirectSound's default mix bins: a mono voice to both fronts, a
	stereo voice's channels to the front left and right */
	stream->mix_left = 1.0f;
	stream->mix_right = 1.0f;
	stream->surround_bins[0][0] = 1.0f;
	stream->surround_bins[stream->channels - 1][1] = 1.0f;
	stream->has_3d = (description->dwFlags & DSSTREAMCAPS_CTRL3D) != 0;
	stream->mode = DS3DMODE_NORMAL;
	stream->minimum_distance = DS3D_DEFAULTMINDISTANCE;
	stream->maximum_distance = DS3D_DEFAULTMAXDISTANCE;
	resampler_reset(stream);
	pthread_mutex_lock(&mixer_lock);
	stream->next = streams;
	streams = stream;
	pthread_mutex_unlock(&mixer_lock);
	*result = &stream->object;
	return DS_OK;
}

/* January-era DirectSound exports the game declares itself
(sound_dsound_xbox.c); the XDK 3911 headers no longer carry them */

void __stdcall DirectSoundStopStream(LPDIRECTSOUNDSTREAM stream)
{
	stream_flush(stream);
}

unsigned long __stdcall DirectSoundGetStreamVoiceStatus(LPDIRECTSOUNDSTREAM stream)
{
	struct sdl_stream *record = stream_from_interface(stream);
	unsigned long active;

	pthread_mutex_lock(&mixer_lock);
	active = record->packet_count != 0;
	pthread_mutex_unlock(&mixer_lock);
	return active;
}

#define STREAM_SETTER(body) \
	struct sdl_stream *record = stream_from_interface(stream); \
	pthread_mutex_lock(&mixer_lock); \
	body; \
	pthread_mutex_unlock(&mixer_lock); \
	return DS_OK;

HRESULT WINAPI IDirectSoundStream_SetFrequency(LPDIRECTSOUNDSTREAM stream, DWORD frequency)
{
	STREAM_SETTER(record->frequency = frequency ? frequency : record->sample_rate)
}

HRESULT WINAPI IDirectSoundStream_SetVolume(LPDIRECTSOUNDSTREAM stream, LONG volume)
{
	STREAM_SETTER(record->volume = gain_from_millibels(volume))
}

/* the speaker bins of a mask, for 5.1, in the order of its set bits: a stereo
voice sends its left channel to the first, third, ... and its right to the
second, fourth, ... (the game's stereo voices on AC-3 take front left, front
right, back left, back right). The crosstalk, reverb and effect sends are not
speakers. volumes is NULL for 0 dB. */
static void stream_set_surround_bins(struct sdl_stream *record, DWORD mix_bin_mask, const LONG *volumes, BOOL clear)
{
	unsigned long bit, index = 0;

	if (clear)
		memset(record->surround_bins, 0, sizeof(record->surround_bins));
	for (bit = 0; bit < 32; bit++)
	{
		if (!(mix_bin_mask & (1UL << bit)))
			continue;
		if ((1UL << bit) & DSMIXBIN_SPEAKER_MASK)
		{
			record->surround_bins[record->channels == 2 ? index & 1 : 0][bit] =
				volumes ? gain_from_millibels(volumes[index]) : 1.0f;
		}
		index++;
	}
}

HRESULT WINAPI IDirectSoundStream_SetMixBins(LPDIRECTSOUNDSTREAM stream, DWORD mix_bin_mask)
{
	STREAM_SETTER(
		record->mix_left = (mix_bin_mask & DSMIXBIN_FRONT_LEFT) ? 1.0f : 0.0f;
		record->mix_right = (mix_bin_mask & DSMIXBIN_FRONT_RIGHT) ? 1.0f : 0.0f;
		stream_set_surround_bins(record, mix_bin_mask, NULL, TRUE))
}

/* volumes come in the order of the set bits of the mask */
HRESULT WINAPI IDirectSoundStream_SetMixBinVolumes(LPDIRECTSOUNDSTREAM stream, DWORD mix_bin_mask, const LONG *volumes)
{
	struct sdl_stream *record = stream_from_interface(stream);
	unsigned long bit, index = 0;

	pthread_mutex_lock(&mixer_lock);
	for (bit = 0; bit < 32; bit++)
	{
		if (!(mix_bin_mask & (1UL << bit)))
			continue;
		if ((1UL << bit) == DSMIXBIN_FRONT_LEFT)
			record->mix_left = gain_from_millibels(volumes[index]);
		else if ((1UL << bit) == DSMIXBIN_FRONT_RIGHT)
			record->mix_right = gain_from_millibels(volumes[index]);
		index++;
	}
	stream_set_surround_bins(record, mix_bin_mask, volumes, FALSE);
	pthread_mutex_unlock(&mixer_lock);
	return DS_OK;
}

HRESULT WINAPI IDirectSoundStream_SetMode(LPDIRECTSOUNDSTREAM stream, DWORD mode, DWORD apply)
{
	(void)apply;
	STREAM_SETTER(record->mode = mode)
}

HRESULT WINAPI IDirectSoundStream_SetPosition(LPDIRECTSOUNDSTREAM stream, FLOAT x, FLOAT y, FLOAT z, DWORD apply)
{
	(void)apply;
	STREAM_SETTER(record->position[0] = x; record->position[1] = y; record->position[2] = z)
}

HRESULT WINAPI IDirectSoundStream_SetMinDistance(LPDIRECTSOUNDSTREAM stream, FLOAT distance, DWORD apply)
{
	(void)apply;
	STREAM_SETTER(record->minimum_distance = distance)
}

HRESULT WINAPI IDirectSoundStream_SetMaxDistance(LPDIRECTSOUNDSTREAM stream, FLOAT distance, DWORD apply)
{
	(void)apply;
	STREAM_SETTER(record->maximum_distance = distance)
}

HRESULT WINAPI IDirectSoundStream_SetI3DL2Source(LPDIRECTSOUNDSTREAM stream, LPCDSI3DL2BUFFER source, DWORD apply)
{
	LONG direct, direct_hf, room, room_hf;

	(void)apply;
	/* the direct path: at low frequencies obstruction and occlusion take
	their LF ratio of their high frequency levels, which (with the source's
	own) muffle it above the HF reference */
	direct = source->lDirect +
		(LONG)(source->Obstruction.lHFLevel * source->Obstruction.flLFRatio) +
		(LONG)(source->Occlusion.lHFLevel * source->Occlusion.flLFRatio);
	if (direct > 0)
		direct = 0;
	direct_hf = source->lDirect + source->lDirectHF + source->Obstruction.lHFLevel + source->Occlusion.lHFLevel;
	/* the room send: occlusion muffles it as it does the direct path, and
	obstruction does not reach it */
	room = source->lRoom + (LONG)(source->Occlusion.lHFLevel * source->Occlusion.flLFRatio);
	room_hf = source->lRoom + source->lRoomHF + source->Occlusion.lHFLevel;
	{
		STREAM_SETTER(
			record->direct = direct;
			record->direct_hf = direct_hf;
			record->room = room;
			record->room_hf = room_hf;
			record->room_rolloff_factor = source->flRoomRolloffFactor)
	}
}

HRESULT WINAPI IDirectSoundStream_Pause(LPDIRECTSOUNDSTREAM stream, DWORD pause)
{
	STREAM_SETTER(record->paused = pause == DSSTREAMPAUSE_PAUSE)
}

HRESULT WINAPI IDirectSoundStream_SetVelocity(LPDIRECTSOUNDSTREAM stream, FLOAT x, FLOAT y, FLOAT z, DWORD apply)
{
	(void)apply;
	STREAM_SETTER(record->velocity[0] = x; record->velocity[1] = y; record->velocity[2] = z)
}

HRESULT WINAPI IDirectSoundStream_SetConeAngles(LPDIRECTSOUNDSTREAM stream, DWORD inside, DWORD outside, DWORD apply) { (void)stream; (void)inside; (void)outside; (void)apply; return DS_OK; }
HRESULT WINAPI IDirectSoundStream_SetConeOrientation(LPDIRECTSOUNDSTREAM stream, FLOAT x, FLOAT y, FLOAT z, DWORD apply) { (void)stream; (void)x; (void)y; (void)z; (void)apply; return DS_OK; }
HRESULT WINAPI IDirectSoundStream_SetConeOutsideVolume(LPDIRECTSOUNDSTREAM stream, LONG volume, DWORD apply) { (void)stream; (void)volume; (void)apply; return DS_OK; }

/* ---------- buffers

The game's only buffer is a silent looping one that keeps the voice
processor busy; it needs no mixing. */

struct null_buffer
{
	ULONG reference_count;
	LPVOID data;
	DWORD size;
	BOOL playing;
};

HRESULT WINAPI DirectSoundCreateBuffer(LPCDSBUFFERDESC description, LPDIRECTSOUNDBUFFER *result)
{
	struct null_buffer *buffer = calloc(1, sizeof(*buffer));

	(void)description;
	if (!buffer)
		return E_OUTOFMEMORY;
	buffer->reference_count = 1;
	*result = (LPDIRECTSOUNDBUFFER)buffer;
	return DS_OK;
}

HRESULT WINAPI IDirectSound_CreateSoundBuffer(LPDIRECTSOUND sound, LPCDSBUFFERDESC description,
	LPDIRECTSOUNDBUFFER *result, LPUNKNOWN outer)
{
	(void)sound;
	(void)outer;
	return DirectSoundCreateBuffer(description, result);
}

ULONG WINAPI IDirectSoundBuffer_Release(LPDIRECTSOUNDBUFFER buffer)
{
	struct null_buffer *record = (struct null_buffer *)buffer;
	ULONG count = --record->reference_count;

	if (!count)
		free(record);
	return count;
}

HRESULT WINAPI IDirectSoundBuffer_SetBufferData(LPDIRECTSOUNDBUFFER buffer, LPVOID data, DWORD size)
{
	struct null_buffer *record = (struct null_buffer *)buffer;

	record->data = data;
	record->size = size;
	return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_Play(LPDIRECTSOUNDBUFFER buffer, DWORD reserved1, DWORD reserved2, DWORD flags)
{
	(void)reserved1;
	(void)reserved2;
	(void)flags;
	((struct null_buffer *)buffer)->playing = TRUE;
	return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_Stop(LPDIRECTSOUNDBUFFER buffer)
{
	((struct null_buffer *)buffer)->playing = FALSE;
	return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_SetCurrentPosition(LPDIRECTSOUNDBUFFER buffer, DWORD play_cursor) { (void)buffer; (void)play_cursor; return DS_OK; }
HRESULT WINAPI IDirectSoundBuffer_SetLoopRegion(LPDIRECTSOUNDBUFFER buffer, DWORD loop_start, DWORD loop_length) { (void)buffer; (void)loop_start; (void)loop_length; return DS_OK; }
HRESULT WINAPI IDirectSoundBuffer_SetPitch(LPDIRECTSOUNDBUFFER buffer, LONG pitch) { (void)buffer; (void)pitch; return DS_OK; }
HRESULT WINAPI IDirectSoundBuffer_SetVolume(LPDIRECTSOUNDBUFFER buffer, LONG volume) { (void)buffer; (void)volume; return DS_OK; }
