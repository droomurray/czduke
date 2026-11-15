/*
 * JFAudioLib output drivers for the M5Stack CardputerZero.
 *
 * This file takes the place of JFAudioLib's driver_sdl.c and
 * driver_fluidsynth.c. Sound effects and MIDI music both leave through one
 * SDL2 audio device: FluidSynth is asked to render the music into the same
 * callback that plays the effects, instead of opening a second sound device
 * of its own (which fails on a board with a single ALSA device and no sound
 * server).
 *
 * FluidSynth is loaded at run time. Without it, or without a soundfont, the
 * game simply has no music.
 *
 * Copyright (C) 2026 czduke contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <glob.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "SDL.h"

#include "asssys.h"
#include "midifuncs.h"
#include "driver_sdl.h"
#include "driver_fluidsynth.h"

/* ------------------------------------------------------------ PCM output */

enum { Err_Ok = 0, Err_Uninitialised, Err_InitSubSystem, Err_OpenAudio, Err_NoSynth, Err_NoSoundFont };

static int pcm_error = Err_Ok;
static int pcm_ready, pcm_playing;
static int pcm_rate, pcm_channels, pcm_bits;

static char *mix_buffer;
static int mix_size, mix_count, mix_current, mix_used;
static void (*mix_callback)(void);

static void music_render(Uint8 *out, int bytes);

static void fill_audio(void *userdata, Uint8 *ptr, int remaining)
{
    Uint8 *start = ptr;
    int total = remaining;

    (void)userdata;
    while (remaining > 0) {
        if (mix_used == mix_size) {
            mix_callback();
            mix_used = 0;
            if (++mix_current >= mix_count) mix_current -= mix_count;
        }
        while (remaining > 0 && mix_used < mix_size) {
            int len = mix_size - mix_used;
            if (remaining < len) len = remaining;
            memcpy(ptr, mix_buffer + (mix_current * mix_size) + mix_used, len);
            ptr += len;
            mix_used += len;
            remaining -= len;
        }
    }
    music_render(start, total);
}

int SDLDrv_GetError(void)
{
    return pcm_error;
}

const char *SDLDrv_ErrorString(int ErrorNumber)
{
    switch (ErrorNumber) {
    case -2: case -1:           return SDLDrv_ErrorString(pcm_error);
    case Err_Ok:                return "SDL Audio ok.";
    case Err_Uninitialised:     return "SDL Audio uninitialised.";
    case Err_InitSubSystem:     return "SDL Audio: error in Init or InitSubSystem.";
    case Err_OpenAudio:         return "SDL Audio: error in OpenAudio.";
    default:                    return "Unknown SDL Audio error code.";
    }
}

int SDLDrv_PCM_Init(int *mixrate, int *numchannels, int *samplebits, void *initdata)
{
    SDL_AudioSpec spec, actual;

    (void)initdata;
    if (pcm_ready) SDLDrv_PCM_Shutdown();

    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        ASS_Message("czaudio: %s\n", SDL_GetError());
        pcm_error = Err_InitSubSystem;
        return -1;
    }
    ASS_Message("czaudio: SDL audio driver: %s\n", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "(none)");

    memset(&spec, 0, sizeof(spec));
    spec.freq = *mixrate;
    spec.format = AUDIO_S16SYS;
    spec.channels = (Uint8)*numchannels;
    spec.callback = fill_audio;
    /* a power of 2, at least 512 samples, about 1/12 s */
    spec.samples = 512;
    while (spec.samples < (4096 * *mixrate / 48000)) spec.samples += spec.samples;

    memset(&actual, 0, sizeof(actual));
    if (SDL_OpenAudio(&spec, &actual) < 0 || actual.freq == 0 ||
        actual.format != AUDIO_S16SYS || actual.channels < 1 || actual.channels > 2) {
        ASS_Message("czaudio: cannot open audio: %s\n", SDL_GetError());
        SDL_CloseAudio();
        pcm_error = Err_OpenAudio;
        return -1;
    }

    *mixrate = pcm_rate = actual.freq;
    *numchannels = pcm_channels = actual.channels;
    *samplebits = pcm_bits = 16;
    pcm_ready = 1;
    return Err_Ok;
}

void SDLDrv_PCM_Shutdown(void)
{
    if (!pcm_ready) return;
    SDL_CloseAudio();
    pcm_ready = pcm_playing = 0;
}

int SDLDrv_PCM_BeginPlayback(char *BufferStart, int BufferSize, int NumDivisions, void (*CallBackFunc)(void))
{
    if (!pcm_ready) {
        pcm_error = Err_Uninitialised;
        return -1;
    }
    if (pcm_playing) SDLDrv_PCM_StopPlayback();

    mix_buffer = BufferStart;
    mix_size = BufferSize;
    mix_count = NumDivisions;
    mix_current = 0;
    mix_used = 0;
    mix_callback = CallBackFunc;

    mix_callback(); /* prime the buffer */
    SDL_PauseAudio(0);
    pcm_playing = 1;
    return Err_Ok;
}

void SDLDrv_PCM_StopPlayback(void)
{
    if (!pcm_ready || !pcm_playing) return;
    SDL_PauseAudio(1);
    pcm_playing = 0;
}

void SDLDrv_PCM_Lock(void)   { SDL_LockAudio(); }
void SDLDrv_PCM_Unlock(void) { SDL_UnlockAudio(); }

/* No CD audio. */
int  SDLDrv_CD_Init(void)                 { return -1; }
void SDLDrv_CD_Shutdown(void)             { }
int  SDLDrv_CD_Play(int track, int loop)  { (void)track; (void)loop; return -1; }
void SDLDrv_CD_Stop(void)                 { }
void SDLDrv_CD_Pause(int pauseon)         { (void)pauseon; }
int  SDLDrv_CD_IsPlaying(void)            { return 0; }
void SDLDrv_CD_SetVolume(int volume)      { (void)volume; }

/* ------------------------------------------------------------ MIDI music */

/* The few FluidSynth 2 entry points used, resolved with dlsym(). */
static struct {
    void *lib;
    void *(*new_settings)(void);
    void  (*delete_settings)(void *);
    int   (*setnum)(void *, const char *, double);
    int   (*setint)(void *, const char *, int);
    void *(*new_synth)(void *);
    void  (*delete_synth)(void *);
    int   (*sfload)(void *, const char *, int);
    int   (*noteon)(void *, int, int, int);
    int   (*noteoff)(void *, int, int);
    int   (*cc)(void *, int, int, int);
    int   (*program_change)(void *, int, int);
    int   (*pitch_bend)(void *, int, int);
    int   (*channel_pressure)(void *, int, int);
    int   (*key_pressure)(void *, int, int, int);
    int   (*system_reset)(void *);
    void  (*set_gain)(void *, float);
    int   (*set_interp_method)(void *, int, int);
    int   (*write_s16)(void *, int, void *, int, int, void *, int, int);
} fs;

static int music_error = Err_Ok;
static void *fs_settings, *fs_synth;
static int fs_rate;
static char soundfont[1024];
static float music_gain = 0.4f;

/* All of this is touched only with the SDL audio lock held. */
static void (*music_service)(void);     /* advances the song by one tick */
static double samples_per_tick, tick_debt;
static int16_t music_buf[1024 * 2];

static int load_fluidsynth(void)
{
    static const char *names[] = { "libfluidsynth.so.3", "libfluidsynth.so.2", "libfluidsynth.so", NULL };
    static const struct { const char *name; void **slot; } syms[] = {
        { "new_fluid_settings", (void **)&fs.new_settings },
        { "delete_fluid_settings", (void **)&fs.delete_settings },
        { "fluid_settings_setnum", (void **)&fs.setnum },
        { "fluid_settings_setint", (void **)&fs.setint },
        { "new_fluid_synth", (void **)&fs.new_synth },
        { "delete_fluid_synth", (void **)&fs.delete_synth },
        { "fluid_synth_sfload", (void **)&fs.sfload },
        { "fluid_synth_noteon", (void **)&fs.noteon },
        { "fluid_synth_noteoff", (void **)&fs.noteoff },
        { "fluid_synth_cc", (void **)&fs.cc },
        { "fluid_synth_program_change", (void **)&fs.program_change },
        { "fluid_synth_pitch_bend", (void **)&fs.pitch_bend },
        { "fluid_synth_channel_pressure", (void **)&fs.channel_pressure },
        { "fluid_synth_key_pressure", (void **)&fs.key_pressure },
        { "fluid_synth_system_reset", (void **)&fs.system_reset },
        { "fluid_synth_set_gain", (void **)&fs.set_gain },
        { "fluid_synth_set_interp_method", (void **)&fs.set_interp_method },
        { "fluid_synth_write_s16", (void **)&fs.write_s16 },
    };

    if (fs.lib) return 0;
    for (int i = 0; names[i] && !fs.lib; i++) fs.lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!fs.lib) return -1;
    for (size_t i = 0; i < sizeof(syms) / sizeof(syms[0]); i++) {
        *syms[i].slot = dlsym(fs.lib, syms[i].name);
        if (!*syms[i].slot) {
            ASS_Message("czaudio: FluidSynth lacks %s\n", syms[i].name);
            dlclose(fs.lib);
            fs.lib = NULL;
            return -1;
        }
    }
    return 0;
}

static int find_soundfont(const char *params)
{
    static const char *patterns[] = {
        "./*.sf2", "/usr/share/sounds/sf2/TimGM6mb.sf2", "/usr/share/sounds/sf2/*.sf2",
        "/usr/share/soundfonts/*.sf2", "/usr/share/sounds/sf3/*.sf3", NULL
    };
    const char *env = getenv("SDL_SOUNDFONTS");
    const char *p;

    soundfont[0] = 0;
    if (params && (p = strstr(params, "soundfont=")) != NULL) {
        snprintf(soundfont, sizeof(soundfont), "%s", p + 10);
        soundfont[strcspn(soundfont, " ")] = 0;
    } else if (env && *env) {
        snprintf(soundfont, sizeof(soundfont), "%s", env);
        soundfont[strcspn(soundfont, ":;")] = 0;
    }
    if (soundfont[0] && access(soundfont, R_OK) == 0) return 1;

    for (int i = 0; patterns[i]; i++) {
        glob_t g;
        memset(&g, 0, sizeof(g));
        if (glob(patterns[i], 0, NULL, &g) == 0 && g.gl_pathc > 0) {
            snprintf(soundfont, sizeof(soundfont), "%s", g.gl_pathv[0]);
            globfree(&g);
            return 1;
        }
        globfree(&g);
    }
    soundfont[0] = 0;
    return 0;
}

static void synth_destroy(void)
{
    if (fs_synth) fs.delete_synth(fs_synth);
    if (fs_settings) fs.delete_settings(fs_settings);
    fs_synth = fs_settings = NULL;
}

/* Settings are chosen for a 1 GHz Cortex-A53: no reverb or chorus, linear
 * interpolation and a modest voice count. */
static int synth_create(int rate)
{
    synth_destroy();
    fs_settings = fs.new_settings();
    if (!fs_settings) return -1;
    fs.setnum(fs_settings, "synth.sample-rate", (double)rate);
    fs.setint(fs_settings, "synth.polyphony", 48);
    fs.setint(fs_settings, "synth.reverb.active", 0);
    fs.setint(fs_settings, "synth.chorus.active", 0);
    fs.setint(fs_settings, "synth.cpu-cores", 1);

    fs_synth = fs.new_synth(fs_settings);
    if (!fs_synth || fs.sfload(fs_synth, soundfont, 1) < 0) {
        synth_destroy();
        return -1;
    }
    fs.set_interp_method(fs_synth, -1, 1);
    fs.set_gain(fs_synth, music_gain);
    fs_rate = rate;
    return 0;
}

static void music_render(Uint8 *out, int bytes)
{
    int frames;

    if (!fs_synth || !music_service || pcm_bits != 16 || samples_per_tick <= 0) return;

    if (fs_rate != pcm_rate && synth_create(pcm_rate) < 0) return; /* effects device was reopened */

    frames = bytes / (2 * pcm_channels);
    while (frames > 0) {
        int n;

        while (tick_debt <= 0 && music_service) {
            music_service();
            tick_debt += samples_per_tick;
        }
        if (!music_service) return;

        n = (int)ceil(tick_debt);
        if (n > frames) n = frames;
        if (n > (int)(sizeof(music_buf) / sizeof(music_buf[0]) / 2)) n = (int)(sizeof(music_buf) / sizeof(music_buf[0]) / 2);
        if (n < 1) n = 1;

        fs.write_s16(fs_synth, n, music_buf, 0, 2, music_buf, 1, 2);

        int16_t *dst = (int16_t *)(void *)out;
        if (pcm_channels == 2) {
            for (int i = 0; i < n * 2; i++) {
                int v = dst[i] + music_buf[i];
                dst[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
        } else {
            for (int i = 0; i < n; i++) {
                int v = dst[i] + (music_buf[i * 2] + music_buf[i * 2 + 1]) / 2;
                dst[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
        }

        out += n * 2 * pcm_channels;
        frames -= n;
        tick_debt -= n;
    }
}

static void Func_NoteOff(int channel, int key, int velocity)   { (void)velocity; fs.noteoff(fs_synth, channel, key); }
static void Func_NoteOn(int channel, int key, int velocity)    { fs.noteon(fs_synth, channel, key, velocity); }
static void Func_PolyAftertouch(int channel, int key, int p)   { fs.key_pressure(fs_synth, channel, key, p); }
static void Func_ControlChange(int channel, int num, int val)  { fs.cc(fs_synth, channel, num, val); }
static void Func_ProgramChange(int channel, int program)       { fs.program_change(fs_synth, channel, program); }
static void Func_ChannelAftertouch(int channel, int pressure)  { fs.channel_pressure(fs_synth, channel, pressure); }
static void Func_PitchBend(int channel, int lsb, int msb)      { fs.pitch_bend(fs_synth, channel, lsb | (msb << 7)); }

static void Func_SetVolume(int volume)
{
    music_gain = .6f - .6f * powf((255.f - (float)volume) / 255.f, 1.66f);
    SDL_LockAudio();
    if (fs_synth) fs.set_gain(fs_synth, music_gain);
    SDL_UnlockAudio();
}

int FluidSynthDrv_GetError(void)
{
    return music_error;
}

const char *FluidSynthDrv_ErrorString(int ErrorNumber)
{
    switch (ErrorNumber) {
    case -2: case -1:           return FluidSynthDrv_ErrorString(music_error);
    case Err_Ok:                return "FluidSynth ok.";
    case Err_Uninitialised:     return "Music needs the SDL sound effects device, which is not open.";
    case Err_NoSynth:           return "FluidSynth is not installed (sudo apt install libfluidsynth3).";
    case Err_NoSoundFont:       return "No usable soundfont (sudo apt install timgm6mb-soundfont).";
    default:                    return "Unknown FluidSynth error.";
    }
}

int FluidSynthDrv_MIDI_Init(midifuncs *funcs, const char *params)
{
    FluidSynthDrv_MIDI_Shutdown();

    if (!pcm_ready)                 music_error = Err_Uninitialised;
    else if (load_fluidsynth() < 0) music_error = Err_NoSynth;
    else if (!find_soundfont(params) || synth_create(pcm_rate) < 0) music_error = Err_NoSoundFont;
    else                            music_error = Err_Ok;

    if (music_error != Err_Ok) {
        ASS_Message("czaudio: no music: %s\n", FluidSynthDrv_ErrorString(music_error));
        return -1;
    }
    ASS_Message("czaudio: music through FluidSynth, soundfont %s\n", soundfont);

    memset(funcs, 0, sizeof(*funcs));
    funcs->NoteOff = Func_NoteOff;
    funcs->NoteOn = Func_NoteOn;
    funcs->PolyAftertouch = Func_PolyAftertouch;
    funcs->ControlChange = Func_ControlChange;
    funcs->ProgramChange = Func_ProgramChange;
    funcs->ChannelAftertouch = Func_ChannelAftertouch;
    funcs->PitchBend = Func_PitchBend;
    funcs->SetVolume = Func_SetVolume;
    return Err_Ok;
}

void FluidSynthDrv_MIDI_Shutdown(void)
{
    if (!fs.lib) return;
    SDL_LockAudio();
    music_service = NULL;
    synth_destroy();
    SDL_UnlockAudio();
}

int FluidSynthDrv_MIDI_StartPlayback(void (*service)(void))
{
    SDL_LockAudio();
    music_service = service;
    tick_debt = 0;
    SDL_UnlockAudio();
    return 0;
}

void FluidSynthDrv_MIDI_HaltPlayback(void)
{
    SDL_LockAudio();
    music_service = NULL;
    if (fs_synth) fs.system_reset(fs_synth); /* silence hanging notes */
    if (fs_synth) fs.set_gain(fs_synth, music_gain);
    SDL_UnlockAudio();
}

void FluidSynthDrv_MIDI_SetTempo(int tempo, int division)
{
    double tps = ((double)tempo * (double)division) / 60.0;
    SDL_LockAudio();
    samples_per_tick = tps > 0 ? (double)pcm_rate / tps : 0;
    SDL_UnlockAudio();
}

void FluidSynthDrv_MIDI_Lock(void)   { SDL_LockAudio(); }
void FluidSynthDrv_MIDI_Unlock(void) { SDL_UnlockAudio(); }
