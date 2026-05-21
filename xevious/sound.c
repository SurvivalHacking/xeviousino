/*
 * Buzzer part start rewrite from: https://github.com/moononournation/esp_8_bit.git
 */
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include <driver/i2s.h>
#include <soc/ledc_struct.h>
#include <esp32-hal-timer.h>

#include "src/nofrendo/nes/nes.h"

#include "hw_config.h"

/* Volume control (0-100), shared with controller.cpp */
volatile int audio_volume = 100; /* default 100% */

#if defined(HW_AUDIO)

#define DEFAULT_FRAGSIZE 128
static void (*audio_callback)(void *buffer, int length) = NULL;

#if defined(HW_AUDIO_EXTDAC)

/* Ring buffer per disaccoppiare audio dal video */
#define AUDIO_RING_SIZE 4096  /* campioni stereo (32 bit ciascuno) */
static int32_t audio_ring[AUDIO_RING_SIZE];
static volatile int ring_head = 0; /* scritto da nofrendo (Core 1) */
static volatile int ring_tail = 0; /* letto dal task audio (Core 0) */
static int16_t *audio_frame;
static int16_t prev_sample = 0; /* per interpolazione lineare */

static int ring_available()
{
	int avail = ring_head - ring_tail;
	if (avail < 0) avail += AUDIO_RING_SIZE;
	return avail;
}

static int ring_free()
{
	return AUDIO_RING_SIZE - 1 - ring_available();
}

/* Task audio su Core 0: legge dal ring buffer e scrive su I2S */
static void audioTask(void *arg)
{
	int32_t out_buf[256];
	while (1)
	{
		int avail = ring_available();
		if (avail > 0)
		{
			int n = avail;
			if (n > 256) n = 256;
			for (int i = 0; i < n; i++)
			{
				out_buf[i] = audio_ring[(ring_tail + i) % AUDIO_RING_SIZE];
			}
			ring_tail = (ring_tail + n) % AUDIO_RING_SIZE;

			size_t bytes_written;
			i2s_write(I2S_NUM_0, (const char *)out_buf, n * 4, &bytes_written, portMAX_DELAY);
		}
		else
		{
			vTaskDelay(1);
		}
	}
}

int osd_init_sound()
{
	audio_frame = NOFRENDO_MALLOC(8 * DEFAULT_FRAGSIZE);

	i2s_config_t cfg = {
		.mode = I2S_MODE_MASTER | I2S_MODE_TX,
		.sample_rate = HW_AUDIO_SAMPLERATE,
		.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
		.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
		.communication_format = I2S_COMM_FORMAT_STAND_I2S,
		.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
		.dma_buf_count = 8,
		.dma_buf_len = 128,
		.use_apll = false,  /* ESP32-S3 NON ha APLL! */
	};
	i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);

	i2s_pin_config_t pins = {
		.mck_io_num = I2S_PIN_NO_CHANGE,  /* MAX98357A non usa MCLK */
		.bck_io_num = HW_AUDIO_EXTDAC_BCLK,
		.ws_io_num = HW_AUDIO_EXTDAC_WCLK,
		.data_out_num = HW_AUDIO_EXTDAC_DOUT,
		.data_in_num = I2S_PIN_NO_CHANGE,
	};
	i2s_set_pin(I2S_NUM_0, &pins);
	i2s_zero_dma_buffer(I2S_NUM_0);

	ring_head = 0;
	ring_tail = 0;
	prev_sample = 0;
	audio_callback = NULL;

	/* Task audio dedicato su Core 0, priorita' alta */
	xTaskCreatePinnedToCore(&audioTask, "audioTask", 4096, NULL, 6, NULL, 0);

	return 0;
}

void osd_stopsound()
{
	audio_callback = NULL;
}

void do_audio_frame()
{
	int left = 22050 / NES_REFRESH_RATE; /* campioni nofrendo per frame */
	int vol = audio_volume;
	while (left)
	{
		int n = DEFAULT_FRAGSIZE;
		if (n > left)
			n = left;
		audio_callback(audio_frame, n);

		/* Upsampling 2x con interpolazione lineare + volume.
		 * Per ogni campione NES produciamo 2 campioni stereo (L=R).
		 * Interpolazione: tra prev_sample e sample[0], tra sample[0] e sample[1], ecc. */
		int free_slots = ring_free();
		int needed = n * 2; /* 2 campioni stereo per ogni campione NES */
		if (needed > free_slots)
			needed = free_slots; /* drop se ring pieno */

		int samples_to_process = needed / 2;
		if (samples_to_process > n) samples_to_process = n;

		for (int i = 0; i < samples_to_process; i++)
		{
			int16_t cur = audio_frame[i];
			/* Campione interpolato (media tra precedente e corrente) */
			int32_t mid_raw = ((int32_t)prev_sample + (int32_t)cur) / 2;

			/* Gain software x4 (compensa GAIN pin scollegato = 9dB min)
			 * + volume utente, con clipping a 16 bit */
			#define SW_GAIN 4
			int32_t s_mid = (mid_raw * SW_GAIN * vol) / 100;  
			int32_t s_cur = (cur * SW_GAIN * vol) / 100;
			/* Clipping a int16 range */
			if (s_mid >  32767) s_mid =  32767;
			if (s_mid < -32768) s_mid = -32768;
			if (s_cur >  32767) s_cur =  32767;
			if (s_cur < -32768) s_cur = -32768;

			/* Scrivi 2 frame stereo (L=R packed in int32) nel ring buffer */
			int32_t frame_mid = ((int32_t)(int16_t)s_mid & 0xFFFF) | ((int32_t)(int16_t)s_mid << 16);
			int32_t frame_cur = ((int32_t)(int16_t)s_cur & 0xFFFF) | ((int32_t)(int16_t)s_cur << 16);
			audio_ring[ring_head] = frame_mid;
			ring_head = (ring_head + 1) % AUDIO_RING_SIZE;
			audio_ring[ring_head] = frame_cur;
			ring_head = (ring_head + 1) % AUDIO_RING_SIZE;
			prev_sample = cur;
		}

		left -= n;
	}
}

void osd_setsound(void (*playfunc)(void *buffer, int length))
{
	audio_callback = playfunc;
}

void osd_getsoundinfo(sndinfo_t *info)
{
	info->sample_rate = 22050; /* nofrendo genera a 22050, noi upsampling a 44100 */
	info->bps = 16;
}

#else  /* !defined(HW_AUDIO_EXTDAC) - DAC interno */

static int16_t *audio_frame;
QueueHandle_t queue;

int osd_init_sound()
{
	audio_frame = NOFRENDO_MALLOC(4 * DEFAULT_FRAGSIZE);

	i2s_config_t cfg = {
		.mode = I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN,
		.sample_rate = HW_AUDIO_SAMPLERATE,
		.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
		.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
		.communication_format = I2S_COMM_FORMAT_PCM | I2S_COMM_FORMAT_I2S_MSB,
		.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
		.dma_buf_count = 10,
		.dma_buf_len = 512,
		.use_apll = true,
	};
	i2s_driver_install(I2S_NUM_0, &cfg, 2, &queue);
	i2s_set_pin(I2S_NUM_0, NULL);
	i2s_set_dac_mode(I2S_DAC_CHANNEL_BOTH_EN);
	i2s_zero_dma_buffer(I2S_NUM_0);

	audio_callback = NULL;
	return 0;
}

void osd_stopsound()
{
	audio_callback = NULL;
}

void do_audio_frame()
{
	int left = HW_AUDIO_SAMPLERATE / NES_REFRESH_RATE;
	while (left)
	{
		int n = DEFAULT_FRAGSIZE;
		if (n > left)
			n = left;
		audio_callback(audio_frame, n);

		int16_t *mono_ptr = audio_frame + n;
		int16_t *stereo_ptr = audio_frame + n + n;
		int i = n;
		while (i--)
		{
			int16_t a = (*(--mono_ptr) >> 3);
			*(--stereo_ptr) = 0x8000 + a;
			*(--stereo_ptr) = 0x8000 - a;
		}

		size_t i2s_bytes_write;
		i2s_write(I2S_NUM_0, (const char *)audio_frame, 4 * n, &i2s_bytes_write, portMAX_DELAY);
		left -= i2s_bytes_write / 4;
	}
}

void osd_setsound(void (*playfunc)(void *buffer, int length))
{
	audio_callback = playfunc;
}

void osd_getsoundinfo(sndinfo_t *info)
{
	info->sample_rate = HW_AUDIO_SAMPLERATE;
	info->bps = 16;
}

#endif /* !defined(HW_AUDIO_EXTDAC) */

#elif defined(HW_AUDIO_BUZZER)

#define DEFAULT_FRAGSIZE (HW_AUDIO_SAMPLERATE / NES_REFRESH_RATE)
static void (*audio_callback)(void *buffer, int length) = NULL;
static int16_t *audio_frame;

static hw_timer_t *timer = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;

volatile uint32_t audio_frame_idx = 0;

void IRAM_ATTR audioSampleTimer()
{
	audio_frame_idx++;
	if (audio_frame_idx >= DEFAULT_FRAGSIZE)
	{
		audio_frame_idx = 0;
	}

	uint16_t s = audio_frame[audio_frame_idx];
	uint8_t raw = s >> 8;
	int16_t boosted = ((int16_t)raw - 128) * 2 + 128;
	if (boosted > 255) boosted = 255;
	if (boosted < 0) boosted = 0;
	ledcWrite(HW_AUDIO_BUZZER_PIN, (uint8_t)boosted);
}

int osd_init_sound()
{
	audio_frame = NOFRENDO_MALLOC(4 * DEFAULT_FRAGSIZE);
	log_d("setup LEDC: pin=%d, f=%d, bit=%d", HW_AUDIO_BUZZER_PIN, 100000, HW_AUDIO_RESOLUTION_BITS);
	ledcAttach(HW_AUDIO_BUZZER_PIN, 100000, HW_AUDIO_RESOLUTION_BITS);
	ledcWrite(HW_AUDIO_BUZZER_PIN, 0);

	// Timer at 4 MHz for better sample rate precision
	timer = timerBegin(4000000);

	// Attach audioSampleTimer function to our timer.
	timerAttachInterrupt(timer, &audioSampleTimer);

	// 4000000 / 181 = 22099 Hz (0.22% error vs 0.78% with 1MHz timer)
	timerAlarm(timer, 181, true, 0);

	return 0;
}

void osd_stopsound()
{
	audio_callback = NULL;
}

void do_audio_frame()
{
	audio_callback(audio_frame, DEFAULT_FRAGSIZE); // get more data
}

void osd_setsound(void (*playfunc)(void *buffer, int length))
{
	// Indicates we should call playfunc() to get more data.
	audio_callback = playfunc;
}

void osd_getsoundinfo(sndinfo_t *info)
{
	info->sample_rate = HW_AUDIO_SAMPLERATE;
	info->bps = 16;
}

#else /* !defined(HW_AUDIO) */

int osd_init_sound()
{
	return 0;
}

void osd_stopsound()
{
}

void do_audio_frame()
{
}

void osd_setsound(void (*playfunc)(void *buffer, int length))
{
}

void osd_getsoundinfo(sndinfo_t *info)
{
	// dummy value
	info->sample_rate = 22050;
	info->bps = 16;
}

#endif /* !defined(HW_AUDIO) */
