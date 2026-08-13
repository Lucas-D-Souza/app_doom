#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "doomtype.h"
#include "i_sound.h"

extern void* W_CacheLumpNum(int lump, int tag);
extern int W_CheckNumForName(const char* name); 
extern int W_LumpLength(int lump); 
#define PU_STATIC 1
#define NUM_CHANNELS 8

typedef struct {
    const uint8_t *data;
    uint32_t length;
    uint32_t position;
    int volume;
    bool playing;
} sfx_channel_t;

static sfx_channel_t sfx_channels[NUM_CHANNELS];
static bool audio_enabled = false;

extern void doom_push_audio_samples(int16_t *samples, size_t num_samples);

static void doom_audio_mixer_task(void *arg) {
    int16_t mix_buffer[512 * 2]; 
    
    while (audio_enabled) {
        for (int i = 0; i < 512; i+=4) {
            int32_t mixed = 0;
            for (int c = 0; c < NUM_CHANNELS; c++) {
                if (sfx_channels[c].playing && sfx_channels[c].data) {
                    if (sfx_channels[c].position < sfx_channels[c].length) {
                        int32_t sample = sfx_channels[c].data[sfx_channels[c].position] - 128;
                        sample = (sample * sfx_channels[c].volume); 
                        mixed += sample;
                        sfx_channels[c].position++;
                    } else {
                        sfx_channels[c].playing = false; 
                    }
                }
            }
            if (mixed > 32767) mixed = 32767;
            else if (mixed < -32768) mixed = -32768;
            
            int16_t final_sample = (int16_t)mixed;
            mix_buffer[(i)*2] = final_sample;     mix_buffer[(i)*2+1] = final_sample;
            mix_buffer[(i+1)*2] = final_sample;   mix_buffer[(i+1)*2+1] = final_sample;
            mix_buffer[(i+2)*2] = final_sample;   mix_buffer[(i+2)*2+1] = final_sample;
            mix_buffer[(i+3)*2] = final_sample;   mix_buffer[(i+3)*2+1] = final_sample;
        }
        doom_push_audio_samples(mix_buffer, 1024);
    }
    vTaskDelete(NULL);
}

void I_InitSound(boolean use_sfx_prefix) {
    audio_enabled = true;
    memset(sfx_channels, 0, sizeof(sfx_channels));
    xTaskCreatePinnedToCore(doom_audio_mixer_task, "doom_mixer", 16384, NULL, 4, NULL, 1);
}

int I_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep) {
    if (!audio_enabled || sfxinfo == NULL) return channel;
    if (sfxinfo->lumpnum <= 0) return channel; 
    uint8_t *raw_data = (uint8_t *)W_CacheLumpNum(sfxinfo->lumpnum, PU_STATIC);
    if (raw_data == NULL) return channel;
    uint32_t real_length = W_LumpLength(sfxinfo->lumpnum);
    if (real_length <= 8) return channel; 
    uint32_t length = raw_data[4] | (raw_data[5] << 8) | (raw_data[6] << 16) | (raw_data[7] << 24);
    if (length > real_length - 8) length = real_length - 8;
    
    sfx_channels[channel].data = raw_data + 8;
    sfx_channels[channel].length = length;
    sfx_channels[channel].position = 0;
    sfx_channels[channel].volume = vol;
    sfx_channels[channel].playing = true;
    return channel;
}

void I_StopSound(int channel) { sfx_channels[channel].playing = false; }
boolean I_SoundIsPlaying(int channel) { return sfx_channels[channel].playing; }
int I_GetSfxLumpNum(sfxinfo_t *sfxinfo) {
    char namebuf[9];
    snprintf(namebuf, sizeof(namebuf), "ds%s", sfxinfo->name);
    return W_CheckNumForName(namebuf); 
}

void I_ShutdownSound(void) { audio_enabled = false; }
void I_UpdateSound(void) { }
void I_UpdateSoundParams(int channel, int vol, int sep) { }
void I_PrecacheSounds(sfxinfo_t *sounds, int num_sounds) { }
void I_InitMusic(void) { }
void I_ShutdownMusic(void) { }
void I_SetMusicVolume(int volume) { }
void I_PauseSong(void) { }
void I_ResumeSong(void) { }
void *I_RegisterSong(void *data, int len) { return NULL; }
void I_UnRegisterSong(void *handle) { }
void I_PlaySong(void *handle, boolean looping) { }
void I_StopSong(void) { }
boolean I_MusicIsPlaying(void) { return false; }
int snd_musicdevice = 0;
int snd_sfxdevice = 0;
int snd_samplerate = 11025;
int snd_cachesize = 0;
int snd_maxslicetime_ms = 0;
char *snd_musiccmd = "";
void I_BindSoundVariables(void) { }