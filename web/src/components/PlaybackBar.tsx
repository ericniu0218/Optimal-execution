import { memo } from 'react';
import { playback, usePlayback } from '../model/playback';

const mmss = (s: number): string => {
  const m = Math.floor(s / 60);
  return `${String(m).padStart(2, '0')}:${String(Math.floor(s % 60)).padStart(2, '0')}`;
};

const SPEEDS = [
  { v: 0.5, t: '0.5×' },
  { v: 1, t: '1×' },
  { v: 2, t: '2×' },
  { v: 4, t: '4×' },
];

function PlaybackBarImpl() {
  const pb = usePlayback();
  const done = pb.t >= pb.duration;
  const frac = pb.duration > 0 ? pb.t / pb.duration : 0;
  return (
    <div className="pbar">
      <button
        type="button"
        className="pbar__play"
        onClick={() => (done ? (playback.seek(0), playback.play()) : playback.toggle())}
        aria-label={pb.playing ? 'Pause replay' : done ? 'Replay from the start' : 'Play replay'}
      >
        {pb.playing ? (
          <svg viewBox="0 0 24 24" aria-hidden="true">
            <rect x="6" y="5" width="4.2" height="14" rx="1" />
            <rect x="13.8" y="5" width="4.2" height="14" rx="1" />
          </svg>
        ) : done ? (
          <svg viewBox="0 0 24 24" aria-hidden="true">
            <path
              d="M12 5a7 7 0 1 1-6.6 4.7"
              fill="none"
              stroke="currentColor"
              strokeWidth="2.2"
              strokeLinecap="round"
            />
            <path d="M4.2 4.6v5.6h5.6z" />
          </svg>
        ) : (
          <svg viewBox="0 0 24 24" aria-hidden="true">
            <path d="M8 5.2v13.6a1 1 0 0 0 1.5.9l11-6.8a1 1 0 0 0 0-1.8l-11-6.8A1 1 0 0 0 8 5.2z" />
          </svg>
        )}
      </button>
      <input
        className="pbar__scrub"
        type="range"
        min={0}
        max={pb.duration}
        step={1}
        value={Math.round(pb.t)}
        aria-label="Replay position"
        style={{ ['--p' as string]: `${frac * 100}%` }}
        onChange={(e) => {
          playback.pause();
          playback.seek(Number(e.target.value));
        }}
      />
      <span className="pbar__time">
        {mmss(pb.t)} <i>/</i> {mmss(pb.duration)}
      </span>
      <div className="pbar__speed" role="group" aria-label="Replay speed">
        {SPEEDS.map((s) => (
          <button
            key={s.v}
            type="button"
            className={pb.speed === s.v ? 'on' : ''}
            aria-pressed={pb.speed === s.v}
            onClick={() => playback.setSpeed(s.v)}
          >
            {s.t}
          </button>
        ))}
      </div>
    </div>
  );
}

export const PlaybackBar = memo(PlaybackBarImpl);
