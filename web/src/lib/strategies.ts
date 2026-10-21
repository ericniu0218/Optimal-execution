import type { StrategyId } from '../engine';

export interface StrategyMeta {
  id: StrategyId;
  /** Short label used in dense UI. */
  short: string;
  /** Full name. */
  label: string;
  /** One line for people who have never heard the term. */
  blurb: string;
  color: string;
  cssVar: string;
}

/**
 * Color follows the entity, never its rank: these four hues are the same ones the
 * README figures use, and nothing else on the page uses them.
 */
export const STRATEGIES: StrategyMeta[] = [
  {
    id: 'AC',
    short: 'AC',
    label: 'Almgren–Chriss',
    blurb: 'Optimal schedule for the dial setting',
    color: '#5aa9ff',
    cssVar: '--c-ac',
  },
  {
    id: 'TWAP',
    short: 'TWAP',
    label: 'TWAP',
    blurb: 'Equal slices every minute',
    color: '#ff8a4c',
    cssVar: '--c-twap',
  },
  {
    id: 'VWAP',
    short: 'VWAP',
    label: 'VWAP',
    blurb: 'More at the open and close',
    color: '#2fd3a0',
    cssVar: '--c-vwap',
  },
  {
    id: 'POV',
    short: 'POV',
    label: 'POV 5%',
    blurb: 'Trade 5% of whatever the market trades',
    color: '#f2b84b',
    cssVar: '--c-pov',
  },
];

export const META: Record<StrategyId, StrategyMeta> = Object.fromEntries(
  STRATEGIES.map((s) => [s.id, s]),
) as Record<StrategyId, StrategyMeta>;

export const STRATEGY_IDS: StrategyId[] = STRATEGIES.map((s) => s.id);

/** Publish the strategy colors as CSS custom properties so CSS and canvas share one source. */
export function installStrategyColors(): void {
  const root = document.documentElement;
  for (const s of STRATEGIES) root.style.setProperty(s.cssVar, s.color);
}
