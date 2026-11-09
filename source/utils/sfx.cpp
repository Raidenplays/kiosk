#include "sfx.hpp"
#include <3ds.h>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#define MUSIC_OFF_FLAG "sdmc:/3ds/Universal-Updater/kiosk-music-off"

namespace {
	struct Clip {
		s16 *data = nullptr;
		u32 samples = 0;
		u32 rate = 22050;
	};

	constexpr int MUSIC_CH = 0, FIRST_SFX_CH = 1, SFX_CHANNELS = 6;
	constexpr float MUSIC_VOL = 0.42f, SFX_VOL = 0.9f;

	bool ready = false, musicOn = true;
	Clip clips[Sfx::COUNT], music;
	ndspWaveBuf sfxBufs[SFX_CHANNELS], musicBuf;
	int nextCh = 0, duckFrames = 0;
	float musicLevel = 0.0f;

	const char *names[Sfx::COUNT] = { "move", "select", "back", "tab", "open", "get", "complete", "pop", "error" };

	/* Minimal 16-bit mono PCM WAV loader (walks chunks, so extra chunks are fine). */
	bool LoadWav(const char *path, Clip &out) {
		FILE *f = fopen(path, "rb");
		if (!f) return false;

		char riff[12];
		if (fread(riff, 1, 12, f) != 12 || memcmp(riff, "RIFF", 4) || memcmp(riff + 8, "WAVE", 4)) { fclose(f); return false; }

		u16 channels = 0, bits = 0;
		u32 rate = 0;
		while (true) {
			char id[4];
			u32 size;
			if (fread(id, 1, 4, f) != 4 || fread(&size, 4, 1, f) != 1) break;

			if (!memcmp(id, "fmt ", 4)) {
				u8 fmt[16];
				if (size < 16 || fread(fmt, 1, 16, f) != 16) break;
				memcpy(&channels, fmt + 2, 2);
				memcpy(&rate, fmt + 4, 4);
				memcpy(&bits, fmt + 14, 2);
				fseek(f, size - 16 + (size & 1), SEEK_CUR);
			} else if (!memcmp(id, "data", 4)) {
				if (channels != 1 || bits != 16 || size == 0) break;
				out.data = (s16 *)linearAlloc(size);
				if (!out.data) break;
				if (fread(out.data, 1, size, f) != size) { linearFree(out.data); out.data = nullptr; break; }
				out.samples = size / 2;
				out.rate = rate;
				DSP_FlushDataCache(out.data, size);
				fclose(f);
				return true;
			} else {
				fseek(f, size + (size & 1), SEEK_CUR);
			}
		}

		fclose(f);
		return false;
	}

	void SetupChannel(int ch, u32 rate, float vol) {
		ndspChnReset(ch);
		ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
		ndspChnSetRate(ch, (float)rate);
		ndspChnSetFormat(ch, NDSP_FORMAT_MONO_PCM16);
		float mix[12] = { 0 };
		mix[0] = mix[1] = vol;
		ndspChnSetMix(ch, mix);
	}

	void StartMusic() {
		if (!music.data) return;
		SetupChannel(MUSIC_CH, music.rate, MUSIC_VOL);
		memset(&musicBuf, 0, sizeof(musicBuf));
		musicBuf.data_vaddr = music.data;
		musicBuf.nsamples = music.samples;
		musicBuf.looping = true;
		ndspChnWaveBufAdd(MUSIC_CH, &musicBuf);
		musicLevel = MUSIC_VOL;
	}
}

void Sfx::Init() {
	if (access("sdmc:/3ds/dspfirm.cdc", F_OK) != 0) return;
	if (R_FAILED(ndspInit())) return;

	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ready = true;

	char path[64];
	for (int i = 0; i < COUNT; i++) {
		snprintf(path, sizeof(path), "romfs:/sfx/%s.wav", names[i]);
		LoadWav(path, clips[i]);
	}
	LoadWav("romfs:/sfx/bgm.wav", music);

	musicOn = access(MUSIC_OFF_FLAG, F_OK) != 0;
	if (musicOn) StartMusic();
}

void Sfx::Exit() {
	if (!ready) return;
	for (int ch = 0; ch < FIRST_SFX_CH + SFX_CHANNELS; ch++) ndspChnWaveBufClear(ch);
	svcSleepThread(20 * 1000 * 1000); // Let the DSP release the buffers before freeing them.
	for (Clip &c : clips) if (c.data) linearFree(c.data);
	if (music.data) linearFree(music.data);
	ndspExit();
	ready = false;
}

bool Sfx::Available() { return ready; }
bool Sfx::MusicOn() { return musicOn; }

void Sfx::Play(Id id) {
	if (!ready || id < 0 || id >= COUNT || !clips[id].data) return;

	const int slot = nextCh;
	nextCh = (nextCh + 1) % SFX_CHANNELS;
	const int ch = FIRST_SFX_CH + slot;

	ndspChnWaveBufClear(ch);
	SetupChannel(ch, clips[id].rate, id == MOVE ? SFX_VOL * 0.6f : SFX_VOL);
	ndspWaveBuf &b = sfxBufs[slot];
	memset(&b, 0, sizeof(b));
	b.data_vaddr = clips[id].data;
	b.nsamples = clips[id].samples;
	b.looping = false;
	ndspChnWaveBufAdd(ch, &b);
}

void Sfx::Duck(int frames) { duckFrames = frames; }

void Sfx::Update() {
	if (!ready || !musicOn) return;
	const float target = duckFrames > 0 ? MUSIC_VOL * 0.25f : MUSIC_VOL;
	if (duckFrames > 0) duckFrames--;
	musicLevel += (target - musicLevel) * 0.08f;

	float mix[12] = { 0 };
	mix[0] = mix[1] = musicLevel;
	ndspChnSetMix(MUSIC_CH, mix);
}

void Sfx::ToggleMusic() {
	musicOn = !musicOn;
	if (musicOn) {
		unlink(MUSIC_OFF_FLAG);
		if (ready) StartMusic();
	} else {
		FILE *f = fopen(MUSIC_OFF_FLAG, "w");
		if (f) fclose(f);
		if (ready) ndspChnWaveBufClear(MUSIC_CH);
	}
}
