import { llround } from './sim';

/** What a strategy may know when a slice fires. Mirrors the C++ ExecutionState. */
export interface SliceState {
  sliceIdx: number;
  remaining: number;
  /** Market trade volume since the parent started, excluding our own fills. */
  marketVolume: number;
}

export interface Strategy {
  name: string;
  /** Child quantity to send now, in [0, remaining]. */
  onSlice(state: SliceState): number;
}

const clamp = (x: number, lo: number, hi: number): number => Math.min(Math.max(x, lo), hi);

/**
 * TWAP, VWAP and Almgren-Chriss are one strategy with different curves: a
 * weight vector w_0..w_N (fraction of the parent still unexecuted at the start
 * of slice j). Each slice targets the PLANNED CUMULATIVE execution and sends the
 * difference from what actually executed, so partial fills and integer rounding
 * roll into later slices instead of silently drifting the schedule.
 */
export function scheduled(name: string, qty: number, weights: number[]): Strategy {
  return {
    name,
    onSlice({ sliceIdx, remaining }) {
      const plannedCum = llround(qty * (1 - weights[sliceIdx + 1]!));
      const executed = qty - remaining;
      return clamp(plannedCum - executed, 0, remaining);
    },
  };
}

/**
 * Percentage of volume: send `participation` x the market volume of the PREVIOUS
 * interval (a strategy cannot see the interval it is entering). The final slice
 * sweeps any remainder so every strategy completes inside the same window.
 */
export function pov(participation: number, slices: number): Strategy {
  let prevVolume = 0;
  return {
    name: `POV(${+(participation * 100).toPrecision(6)}%)`,
    onSlice({ sliceIdx, remaining, marketVolume }) {
      if (sliceIdx === slices - 1) return remaining;
      const interval = marketVolume - prevVolume;
      prevVolume = marketVolume;
      return clamp(llround(participation * interval), 0, remaining);
    },
  };
}
