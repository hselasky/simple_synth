/*-
 * Copyright (c) 2010-2021 Hans Petter Selasky. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <err.h>

#include <sys/soundcard.h>

static uint8_t base_key = 60;		/* C4 */
static uint8_t base_chan = 0;
static uint16_t base_hz = 440;
static uint8_t octave_size = 12;
static uint32_t sample_rate = 48000;	/* 48KHz */
static char *midi_dev = "/dev/umidi0.0";
static char *oss_dev = "/dev/dsp";
static int midi_file = -1;
static int oss_file = -1;
static uint32_t buf_size = 48000 / 50;
static uint8_t pedal_on = 0;
static uint8_t amplitude_last[128];
static uint8_t amplitude_curr[128];
static pthread_mutex_t Giant;
static float wave_offset[128];
static pthread_t midi_thread;

static uint8_t
midi_read_byte(void)
{
	int err;
	uint8_t buf[1];

top:
	while (midi_file < 0) {

		usleep(250000);

		midi_file = open(midi_dev, O_RDONLY);
		if (midi_file < 0)
			continue;
	}

	err = read(midi_file, buf, sizeof(buf));

	if (err == 0)
		goto top;

	if (err != sizeof(buf)) {
		close(midi_file);
		midi_file = -1;
		goto top;
	}
	return (buf[0]);
}

static void
midi_notes_off(void)
{
	pthread_mutex_lock(&Giant);
	memset(amplitude_curr, 0, sizeof(amplitude_curr));
	pedal_on = 0;
	pthread_mutex_unlock(&Giant);
}

static void *
midi_read_thread(void *arg)
{
	uint8_t temp;
	uint8_t chan;
	uint8_t key;
	uint8_t vel;

	while (1) {

		temp = midi_read_byte();
		if (!(temp & 0x80))
			continue;

		chan = temp & 0x0F;
		if (chan != base_chan)
			continue;

		if (temp == 0xFF)
			midi_notes_off();

		switch (temp & 0x70) {
		case 0x00:
			key = midi_read_byte() & 0x7F;
			pthread_mutex_lock(&Giant);
			amplitude_curr[key] = 0;
			pthread_mutex_unlock(&Giant);
			break;
		case 0x10:
			key = midi_read_byte() & 0x7F;
			vel = midi_read_byte() & 0x7F;
			pthread_mutex_lock(&Giant);
			amplitude_curr[key] = vel;
			pthread_mutex_unlock(&Giant);
			break;
		case 0x30:
			key = midi_read_byte() & 0x7F;
			vel = midi_read_byte() & 0x7F;
			switch (key) {
			case 0x40:	/* pedal */
				pedal_on = vel;
				break;
			case 0x78:
			case 0x79:
			case 0x7A:
				midi_notes_off();
				break;
			default:
				break;
			}
			break;
		default:
			break;
		}
	}
}

static float
wave_function_16(float _x, float _power)
{
	uint16_t x = (_x - floorf(_x)) * (1U << 16);
	float retval;
	uint8_t num;

	/* Handle special cases, if any */
	switch (x) {
	case 0xffff:
	case 0x0000:
		return (1.0f);
	case 0x3fff:
	case 0x4000:
	case 0xBfff:
	case 0xC000:
		return (0.0f);
	case 0x7FFF:
	case 0x8000:
		return (-1.0f);
	default:
		break;
	}

	/* Apply "grey" encoding */
	for (uint16_t mask = 1U << 15; mask != 1; mask /= 2) {
		if (x & mask)
			x ^= (mask - 1);
	}

	/* Find first set bit */
	for (num = 0; num != 14; num++) {
		if (x & (1U << num)) {
			num++;
			break;
		}
	}

	/* Initialize return value */
	retval = 0.0;

	/* Compute the rest of the power series */
	for (; num != 14; num++) {
		if (x & (1U << num)) {
			retval = (1.0f - retval) / 2.0f;
			retval = powf(retval, _power);
		} else {
			retval = (1.0f + retval) / 2.0f;
			retval = powf(retval, _power);
		}
	}

	/* Check if halfway */
	if (x & (1ULL << 14))
		retval = -retval;

	return (retval);
}

static void
generate_audio(int32_t *pbuf, uint32_t nsamples)
{
	uint32_t n;
	uint8_t j;
	float fade_fact;
	float curr_fact;
	float freq;

	memset(pbuf, 0, sizeof(pbuf[0]) * nsamples);

	pthread_mutex_lock(&Giant);

	for (j = 0; j != 128; j++) {

		if ((amplitude_curr[j] == 0) &&
		    (amplitude_last[j] == 0))
			continue;

		curr_fact = amplitude_last[j];

		if (amplitude_curr[j] != amplitude_last[j]) {
			fade_fact = (float)((int)amplitude_curr[j] -
			    (int)amplitude_last[j]) / (float)nsamples;
		} else {
			fade_fact = 0.0f;
		}

		if (pedal_on && (fade_fact < 0))
			fade_fact = 0.0f;

		freq = ((float)base_hz) * powf(2.0f, (float)((int)j -
		    (int)base_key) / (float)octave_size);

		if (freq >= (sample_rate / 2.0f))
			freq = (sample_rate / 2.0f);

		freq /= (float)sample_rate;

		for (n = 0; n != nsamples; n++) {

			pbuf[n] += ((1LL << 28) / 128.0f) * curr_fact *
			    wave_function_16(wave_offset[j], 1.0f / (1.0f + curr_fact / 64.0f));

			/* advance phase */
			wave_offset[j] += freq;

			/* avoid overflow */
			wave_offset[j] -= floorf(wave_offset[j]);

			curr_fact += fade_fact;
		}

		if (curr_fact < 1)
			curr_fact = 0;
		if (curr_fact > 126)
			curr_fact = 127;

		if (pedal_on != 0)
			amplitude_last[j] = (int)curr_fact;
		else
			amplitude_last[j] = amplitude_curr[j];
	}

	pthread_mutex_unlock(&Giant);
}

static void *
oss_write_thread(void *arg)
{
	const int fmt = AFMT_S32_NE;
	const int chn = 1;

	int err;
	int rem;
	int odly;

	int32_t buf[buf_size];

	while (1) {

		usleep(250000);

		oss_file = open(oss_dev, O_WRONLY | O_NONBLOCK);
		if (oss_file < 0)
			continue;

		err = ioctl(oss_file, SOUND_PCM_WRITE_RATE, &sample_rate);
		if (err < 0) {
			warn("Could not set audio rate\n");
			close(oss_file);
			continue;
		}

		err = ioctl(oss_file, SNDCTL_DSP_SETFMT, &fmt);
		if (err < 0) {
			warn("Could not set audio format\n");
			close(oss_file);
			continue;
		}

		err = ioctl(oss_file, SOUND_PCM_WRITE_CHANNELS, &chn);
		if (err < 0) {
			warn("Could not set number of channels\n");
			close(oss_file);
			continue;
		}

		const int blk = sizeof(buf);

		err = ioctl(oss_file, SNDCTL_DSP_SETBLKSIZE, &blk);
		if (err < 0) {
			warn("Could not set number of channels\n");
			close(oss_file);
			continue;
		}

		rem = 0;

		while (1) {

			if (rem == 0) {
				rem = buf_size;
				generate_audio(buf, buf_size);
			}
			odly = 0;

			err = ioctl(oss_file, SNDCTL_DSP_GETODELAY, &odly);

			if (odly < sizeof(buf)) {
				err = write(oss_file, buf + buf_size - rem, sizeof(buf[0]) * rem);
			} else {
				err = -1;
				errno = EWOULDBLOCK;
			}

			if (err < (int)sizeof(buf[0])) {
				if (errno == EWOULDBLOCK) {
					usleep(1000000 / 100);
				} else {
					close(oss_file);
					break;
				}
			} else {
				rem -= err / sizeof(buf[0]);
			}
		}
	}
}

static void
usage(void)
{
	fprintf(stderr, "Usage: simple_synth [parameters]\n"
	    "\t" "-k <base_key (60=C5)>\n"
	    "\t" "-H <base_hz (440Hz)>\n"
	    "\t" "-o <octave_size (12)>\n"
	    "\t" "-r <sample_rate (48000Hz)>\n"
	    "\t" "-d <MIDI device (/dev/umidi0.0)>\n"
	    "\t" "-f <OSS device (/dev/dsp)>\n");

	exit(1);
}

int
main(int argc, char **argv)
{
	int c;

	while ((c = getopt(argc, argv, "f:k:H:o:r:w:d:n:h")) != -1) {
		switch (c) {
		case 'k':
			base_key = atoi(optarg);
			break;
		case 'H':
			base_hz = atoi(optarg);
			break;
		case 'o':
			octave_size = atoi(optarg);
			if (octave_size == 0)
				err(1, "-o option requires non-zero value\n");
			break;
		case 'r':
			sample_rate = atoi(optarg);
			buf_size = sample_rate / 50;
			break;
		case 'd':
			midi_dev = optarg;
			break;
		case 'f':
			oss_dev = optarg;
			break;
		default:
			usage();
		}
	}

	pthread_mutex_init(&Giant, NULL);

	pthread_create(&midi_thread, NULL, &midi_read_thread, NULL);

	oss_write_thread(NULL);

	return (0);			/* not reached */
}
