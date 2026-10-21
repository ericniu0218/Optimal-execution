import { useSyncExternalStore } from 'react';

/**
 * The replay clock. Lives outside React so the canvases can follow it at frame
 * rate while the rest of the tree re-renders only when something real changes.
 * `t` is seconds into the replayed window.
 */
export interface PlaybackState {
  t: number;
  duration: number;
  playing: boolean;
  /** Multiplier on the base pace (a whole window plays in BASE_SECONDS / speed). */
  speed: number;
}

/** Wall-clock seconds a full window takes at 1x. */
const BASE_SECONDS = 26;
/** UI updates are throttled to ~30 Hz; the clock itself integrates every frame. */
const PUBLISH_MS = 33;

class PlaybackStore {
  private state: PlaybackState = { t: 0, duration: 1800, playing: false, speed: 1 };
  private listeners = new Set<() => void>();
  private raf = 0;
  private last = 0;
  private lastPublish = 0;
  private t = 0;

  subscribe = (fn: () => void): (() => void) => {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  };

  getSnapshot = (): PlaybackState => this.state;

  private publish(force = false): void {
    const now = performance.now();
    if (!force && now - this.lastPublish < PUBLISH_MS) return;
    this.lastPublish = now;
    this.state = { ...this.state, t: this.t };
    this.listeners.forEach((l) => l());
  }

  private loop = (now: number): void => {
    if (!this.state.playing) return;
    const dt = (now - this.last) / 1000;
    this.last = now;
    const pace = this.state.duration / BASE_SECONDS;
    this.t = Math.min(this.state.duration, this.t + dt * pace * this.state.speed);
    if (this.t >= this.state.duration) {
      this.state = { ...this.state, playing: false };
      this.publish(true);
      return;
    }
    this.publish();
    this.raf = requestAnimationFrame(this.loop);
  };

  play = (): void => {
    if (this.state.playing) return;
    if (this.t >= this.state.duration) this.t = 0;
    this.state = { ...this.state, playing: true };
    this.last = performance.now();
    this.publish(true);
    this.raf = requestAnimationFrame(this.loop);
  };

  pause = (): void => {
    cancelAnimationFrame(this.raf);
    this.state = { ...this.state, playing: false };
    this.publish(true);
  };

  toggle = (): void => (this.state.playing ? this.pause() : this.play());

  seek = (t: number): void => {
    this.t = Math.min(this.state.duration, Math.max(0, t));
    this.publish(true);
  };

  /** New window: rewind, and optionally start playing. */
  reset = (duration: number, autoplay: boolean): void => {
    cancelAnimationFrame(this.raf);
    this.t = 0;
    this.state = { ...this.state, duration, playing: false, t: 0 };
    this.publish(true);
    if (autoplay) this.play();
  };

  /** Jump to the finished state (reduced-motion users, or "skip to the end"). */
  finish = (): void => {
    cancelAnimationFrame(this.raf);
    this.t = this.state.duration;
    this.state = { ...this.state, playing: false };
    this.publish(true);
  };

  setSpeed = (speed: number): void => {
    this.state = { ...this.state, speed };
    this.publish(true);
  };
}

export const playback = new PlaybackStore();

export function usePlayback(): PlaybackState {
  return useSyncExternalStore(playback.subscribe, playback.getSnapshot);
}
