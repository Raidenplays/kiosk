/*
	Kiosk sound layer: short effects on rotating channels plus a looping music track.
	Needs sdmc:/3ds/dspfirm.cdc (dumped by DSP1); without it everything stays silent.
*/
#ifndef _KIOSK_SFX_HPP
#define _KIOSK_SFX_HPP

namespace Sfx {
	enum Id { MOVE, SELECT, BACK, TAB, OPEN, GET, COMPLETE, POP, ERROR, COUNT };

	void Init();
	void Exit();
	void Play(Id id);
	void Update(); // Call once per frame (music ducking).
	void Duck(int frames); // Lower the music for a moment, e.g. under a jingle.
	bool Available();
	bool MusicOn();
	void ToggleMusic();
};

#endif
