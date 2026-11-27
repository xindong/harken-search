#pragma once

// Stop → fade-out → real stop → cursor reset is a small state machine shared by
// AudioEngine::stop(), seekToPosition() and getPlaybackPosition(). Keeping the
// two decisions here makes the ordering testable without an audio device.

namespace AudioStopReset {

// A pending stop owns the cursor reset. seek(0) issued in the same tick as the
// stop (the transport Stop shortcut) must not clear the scheduled stop time,
// otherwise the fade is cancelled and playback resumes from zero.
inline bool ShouldDeferSeekWhileStopPending(bool stopPending, float normalizedPos) {
    return stopPending && normalizedPos <= 0.0f;
}

// The cursor reset is only safe once the backend has actually stopped; while
// the fade is still running the sound keeps playing from its current cursor.
inline bool ShouldCompleteCursorReset(bool stopPending, bool backendPlaying) {
    return stopPending && !backendPlaying;
}

} // namespace AudioStopReset

