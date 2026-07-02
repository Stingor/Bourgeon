//
// i_dsound.c — BOURGEON platform sound backend for the embedded doomgeneric.
//
// SFX via DirectSound8: the host RO client already loads dsound.dll, and
// DirectSound mixes secondary buffers natively, so each DOOM channel simply
// becomes one secondary buffer (8-bit mono PCM straight from the DMX lumps,
// per-buffer volume/pan). No SDL, no resampling, no extra threads — StartSound
// creates+plays a buffer, Update() reaps finished ones.
//
// Music is a valid no-op module for now (MIDI playback would need mus2mid +
// midiStream — a future project). i_sound.c only needs the pointers to exist.
//
// This file is plain C, compiled into the doomgeneric static lib, and is the
// FEATURE_SOUND counterpart of the excluded i_sdlsound/i_sdlmusic backends.
//

// dsound.h pulls COM headers (objbase.h -> rpcndr.h) whose `boolean` typedef
// clashes with doomtype.h's enum. Rename the Windows one out of the way for
// the duration of the system includes — nothing we use references it.
#define boolean win_rpcndr_boolean
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>  // WAVEFORMATEX etc. (skipped by LEAN_AND_MEAN, dsound.h needs it)
#include <dsound.h>
#undef boolean
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

// Demanded by i_sound.c's I_BindSoundVariables under FEATURE_SOUND (SDL port
// resampling knobs — meaningless here, but the config binding needs storage).
int   use_libsamplerate  = 0;
float libsamplerate_scale = 1.0f;

// Provided by the Bourgeon plugin (doom_tweaks.cc): the game's HWND for
// DirectSound's cooperative level.
extern void* DG_GetGameWindow(void);

#define NUM_DS_CHANNELS 16

static LPDIRECTSOUND8       g_ds = NULL;
static LPDIRECTSOUNDBUFFER  g_chan[NUM_DS_CHANNELS];
static boolean              g_use_prefix = true;

// ── helpers ───────────────────────────────────────────────────────────────────

static void ReleaseChannel(int channel)
{
    if (g_chan[channel] != NULL)
    {
        IDirectSoundBuffer_Stop(g_chan[channel]);
        IDirectSoundBuffer_Release(g_chan[channel]);
        g_chan[channel] = NULL;
    }
}

// vol 0..127 -> logarithmic DirectSound attenuation (hundredths of dB),
// sep 0..254 (127 = centre) -> pan. Full ±10000 pan is jarring; scale it down.
static void ApplyParams(LPDIRECTSOUNDBUFFER buf, int vol, int sep)
{
    LONG att;
    if (vol <= 0)
        att = DSBVOLUME_MIN;
    else
    {
        att = (LONG)(2000.0 * log10((double)vol / 127.0));
        if (att < DSBVOLUME_MIN) att = DSBVOLUME_MIN;
    }
    IDirectSoundBuffer_SetVolume(buf, att);
    IDirectSoundBuffer_SetPan(buf, (LONG)(((double)(sep - 127) / 127.0) * 5000.0));
}

// ── sound_module_t implementation ─────────────────────────────────────────────

static boolean I_DS_Init(boolean use_sfx_prefix)
{
    HWND hwnd = (HWND)DG_GetGameWindow();

    g_use_prefix = use_sfx_prefix;
    memset(g_chan, 0, sizeof(g_chan));

    if (FAILED(DirectSoundCreate8(NULL, &g_ds, NULL)))
    {
        g_ds = NULL;
        return false;
    }
    if (FAILED(IDirectSound8_SetCooperativeLevel(
            g_ds, hwnd != NULL ? hwnd : GetDesktopWindow(), DSSCL_PRIORITY)))
    {
        IDirectSound8_Release(g_ds);
        g_ds = NULL;
        return false;
    }
    printf("I_DS_Init: DirectSound SFX backend initialised.\n");
    return true;
}

static void I_DS_Shutdown(void)
{
    int i;
    for (i = 0; i < NUM_DS_CHANNELS; ++i)
        ReleaseChannel(i);
    if (g_ds != NULL)
    {
        IDirectSound8_Release(g_ds);
        g_ds = NULL;
    }
}

static int I_DS_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char namebuf[16];
    if (sfx->link != NULL)
        sfx = sfx->link;
    if (g_use_prefix)
        snprintf(namebuf, sizeof(namebuf), "ds%s", sfx->name);
    else
        snprintf(namebuf, sizeof(namebuf), "%s", sfx->name);
    return W_GetNumForName(namebuf);
}

// Reap finished buffers so slots don't pin memory. Called once per tic.
static void I_DS_Update(void)
{
    int i;
    for (i = 0; i < NUM_DS_CHANNELS; ++i)
    {
        if (g_chan[i] != NULL)
        {
            DWORD status = 0;
            if (FAILED(IDirectSoundBuffer_GetStatus(g_chan[i], &status)) ||
                (status & DSBSTATUS_PLAYING) == 0)
                ReleaseChannel(i);
        }
    }
}

static void I_DS_UpdateSoundParams(int channel, int vol, int sep)
{
    if (channel < 0 || channel >= NUM_DS_CHANNELS || g_chan[channel] == NULL)
        return;
    ApplyParams(g_chan[channel], vol, sep);
}

// DMX lump: u16 format (=3), u16 sample rate, u32 length, then the samples
// bracketed by 16 padding bytes on each side (skipped to avoid pops).
static int I_DS_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep)
{
    int             lumpnum;
    unsigned int    lumplen, len, rate, pcmlen;
    const byte     *data, *pcm;
    WAVEFORMATEX    wf;
    DSBUFFERDESC    desc;
    LPDIRECTSOUNDBUFFER buf = NULL;
    void  *p1, *p2;
    DWORD  n1, n2;

    if (g_ds == NULL || channel < 0 || channel >= NUM_DS_CHANNELS)
        return -1;

    ReleaseChannel(channel);

    lumpnum = I_DS_GetSfxLumpNum(sfxinfo);
    lumplen = W_LumpLength(lumpnum);
    data = W_CacheLumpNum(lumpnum, PU_STATIC);

    if (lumplen < 8 || data[0] != 0x03 || data[1] != 0x00)
    {
        W_ReleaseLumpNum(lumpnum);
        return -1;
    }
    rate = data[2] | (data[3] << 8);
    len = data[4] | (data[5] << 8) | (data[6] << 16) |
          ((unsigned int)data[7] << 24);
    if (len > lumplen - 8)
        len = lumplen - 8;
    if (len > 32)
    {
        pcm = data + 8 + 16;
        pcmlen = len - 32;
    }
    else
    {
        pcm = data + 8;
        pcmlen = len;
    }
    if (pcmlen == 0 || rate == 0)
    {
        W_ReleaseLumpNum(lumpnum);
        return -1;
    }

    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;   // DMX samples are 8-bit unsigned mono
    wf.nChannels = 1;
    wf.nSamplesPerSec = rate;          // typically 11025 Hz
    wf.wBitsPerSample = 8;
    wf.nBlockAlign = 1;
    wf.nAvgBytesPerSec = rate;

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN | DSBCAPS_GLOBALFOCUS;
    desc.dwBufferBytes = pcmlen;
    desc.lpwfxFormat = &wf;

    if (FAILED(IDirectSound8_CreateSoundBuffer(g_ds, &desc, &buf, NULL)))
    {
        W_ReleaseLumpNum(lumpnum);
        return -1;
    }
    if (SUCCEEDED(IDirectSoundBuffer_Lock(buf, 0, pcmlen, &p1, &n1, &p2, &n2, 0)))
    {
        memcpy(p1, pcm, n1);
        if (p2 != NULL && n2 != 0)
            memcpy(p2, pcm + n1, n2);
        IDirectSoundBuffer_Unlock(buf, p1, n1, p2, n2);
    }
    W_ReleaseLumpNum(lumpnum);  // buffer holds its own copy now

    ApplyParams(buf, vol, sep);
    IDirectSoundBuffer_Play(buf, 0, 0, 0);
    g_chan[channel] = buf;
    return channel;
}

static void I_DS_StopSound(int channel)
{
    if (channel < 0 || channel >= NUM_DS_CHANNELS)
        return;
    ReleaseChannel(channel);
}

static boolean I_DS_SoundIsPlaying(int channel)
{
    DWORD status = 0;
    if (channel < 0 || channel >= NUM_DS_CHANNELS || g_chan[channel] == NULL)
        return false;
    if (FAILED(IDirectSoundBuffer_GetStatus(g_chan[channel], &status)))
        return false;
    return (status & DSBSTATUS_PLAYING) != 0;
}

static snddevice_t sound_ds_devices[] =
{
    SNDDEVICE_SB,
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_AWE32,
};

sound_module_t DG_sound_module =
{
    sound_ds_devices,
    arrlen(sound_ds_devices),
    I_DS_Init,
    I_DS_Shutdown,
    I_DS_GetSfxLumpNum,
    I_DS_Update,
    I_DS_UpdateSoundParams,
    I_DS_StartSound,
    I_DS_StopSound,
    I_DS_SoundIsPlaying,
    NULL,  // CacheSounds (lazy loading is fine — lumps are tiny)
};

// ── music: valid no-op module (MIDI = future project) ─────────────────────────

static boolean I_DSMus_Init(void) { return true; }
static void    I_DSMus_Shutdown(void) {}
static void    I_DSMus_SetMusicVolume(int volume) { (void)volume; }
static void    I_DSMus_PauseMusic(void) {}
static void    I_DSMus_ResumeMusic(void) {}
static void   *I_DSMus_RegisterSong(void *data, int len)
{
    (void)data; (void)len;
    return NULL;
}
static void    I_DSMus_UnRegisterSong(void *handle) { (void)handle; }
static void    I_DSMus_PlaySong(void *handle, boolean looping)
{
    (void)handle; (void)looping;
}
static void    I_DSMus_StopSong(void) {}
static boolean I_DSMus_MusicIsPlaying(void) { return false; }

static snddevice_t music_ds_devices[] =
{
    SNDDEVICE_SB,
    SNDDEVICE_GENMIDI,
    SNDDEVICE_GUS,
    SNDDEVICE_AWE32,
};

music_module_t DG_music_module =
{
    music_ds_devices,
    arrlen(music_ds_devices),
    I_DSMus_Init,
    I_DSMus_Shutdown,
    I_DSMus_SetMusicVolume,
    I_DSMus_PauseMusic,
    I_DSMus_ResumeMusic,
    I_DSMus_RegisterSong,
    I_DSMus_UnRegisterSong,
    I_DSMus_PlaySong,
    I_DSMus_StopSong,
    I_DSMus_MusicIsPlaying,
    NULL,  // Poll (i_sound.c null-checks it)
};
