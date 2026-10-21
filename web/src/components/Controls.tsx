import { memo, useRef, type KeyboardEvent } from 'react';
import { clockHm, fixed, int, signed } from '../lib/format';
import { META } from '../lib/strategies';

// ---- segmented control -----------------------------------------------------------

interface SegProps<T extends number> {
  label: string;
  value: T;
  options: Array<{ value: T; text: string }>;
  onChange: (v: T) => void;
}

export function Segmented<T extends number>({ label, value, options, onChange }: SegProps<T>) {
  const onKey = (e: KeyboardEvent<HTMLDivElement>) => {
    const i = options.findIndex((o) => o.value === value);
    let n = i;
    if (e.key === 'ArrowRight' || e.key === 'ArrowDown') n = Math.min(options.length - 1, i + 1);
    else if (e.key === 'ArrowLeft' || e.key === 'ArrowUp') n = Math.max(0, i - 1);
    else return;
    e.preventDefault();
    onChange(options[n]!.value);
  };
  return (
    <div className="seg" role="radiogroup" aria-label={label} onKeyDown={onKey}>
      {options.map((o) => (
        <button
          key={o.value}
          type="button"
          role="radio"
          aria-checked={o.value === value}
          tabIndex={o.value === value ? 0 : -1}
          className={`seg__btn${o.value === value ? ' seg__btn--on' : ''}`}
          onClick={() => onChange(o.value)}
        >
          {o.text}
        </button>
      ))}
    </div>
  );
}

// ---- order card -----------------------------------------------------------------

export const QTY_OPTIONS = [
  { value: 5000, text: '5k' },
  { value: 10000, text: '10k' },
  { value: 20000, text: '20k' },
  { value: 40000, text: '40k' },
];
export const MIN_OPTIONS = [
  { value: 15, text: '15 min' },
  { value: 30, text: '30 min' },
  { value: 60, text: '60 min' },
];

interface OrderProps {
  qty: number;
  minutes: number;
  arrivalPrice: number; // dollars
  participation: number; // 0..1
  onQty: (q: number) => void;
  onMinutes: (m: number) => void;
}

function OrderImpl({ qty, minutes, arrivalPrice, participation, onQty, onMinutes }: OrderProps) {
  const notional = qty * arrivalPrice;
  return (
    <div className="order">
      <div className="field">
        <span className="field__label" id="lbl-qty">
          Shares to sell
        </span>
        <Segmented label="Shares to sell" value={qty} options={QTY_OPTIONS} onChange={onQty} />
      </div>
      <div className="field">
        <span className="field__label" id="lbl-min">
          Finish within
        </span>
        <Segmented label="Finish within" value={minutes} options={MIN_OPTIONS} onChange={onMinutes} />
      </div>
      <p className="order__sum">
        Sell <b>{int(qty)}</b> AAPL, about <b>${(notional / 1e6).toFixed(2)}M</b>, over <b>{minutes} min</b>.
        That is <b>{fixed(participation * 100, 1)}%</b> of everything the market traded in the window.
      </p>
      <dl className="terms">
        <div>
          <dt>bps</dt>
          <dd>A basis point is 0.01% of the order&apos;s value.</dd>
        </div>
        <div>
          <dt>Shortfall</dt>
          <dd>
            How much worse the average sale price was than the price when the order started. Lower is better;
            negative means it beat that price.
          </dd>
        </div>
      </dl>
    </div>
  );
}
export const Order = memo(OrderImpl);

// ---- replay-window picker ----------------------------------------------------------

interface PickerProps {
  startMins: number[];
  /** twapIS - acIS per window, bps. Positive means AC was cheaper in that window. */
  advantage: number[];
  selected: number;
  minutes: number;
  openSec: number;
  onPick: (startMin: number) => void;
}

function PickerImpl({ startMins, advantage, selected, minutes, openSec, onPick }: PickerProps) {
  const ref = useRef<HTMLDivElement>(null);
  const max = Math.max(1, ...advantage.map((a) => Math.abs(a)));
  const selIdx = startMins.indexOf(selected);

  const pickIdx = (i: number) => onPick(startMins[Math.min(startMins.length - 1, Math.max(0, i))]!);

  const sorted = advantage.map((a, i) => ({ a, i })).sort((x, y) => x.a - y.a);
  const median = sorted[Math.floor(sorted.length / 2)]!.i;
  const best = sorted[sorted.length - 1]!.i;
  const worst = sorted[0]!.i;
  const wins = advantage.filter((a) => a > 0).length;

  return (
    <div className="picker">
      <div
        ref={ref}
        className="picker__strip"
        role="listbox"
        aria-label="Replay window start time"
        tabIndex={0}
        onKeyDown={(e) => {
          if (e.key === 'ArrowRight') {
            e.preventDefault();
            pickIdx(selIdx + 1);
          } else if (e.key === 'ArrowLeft') {
            e.preventDefault();
            pickIdx(selIdx - 1);
          }
        }}
      >
        {startMins.map((m, i) => {
          const a = advantage[i]!;
          const h = Math.max(2, (Math.abs(a) / max) * 50);
          const isSel = m === selected;
          return (
            <button
              key={m}
              type="button"
              role="option"
              aria-selected={isSel}
              tabIndex={-1}
              className={`picker__bar${isSel ? ' picker__bar--on' : ''}`}
              title={`${clockHm(openSec + m * 60)} ET: AC was ${signed(Math.abs(a), 1)} bps ${a >= 0 ? 'cheaper' : 'costlier'} than TWAP`}
              onClick={() => onPick(m)}
            >
              <span
                className="picker__fill"
                style={
                  a >= 0
                    ? { height: `${h}%`, background: META.AC.color, bottom: '50%' }
                    : { height: `${h}%`, background: META.TWAP.color, top: '50%' }
                }
              />
            </button>
          );
        })}
        <span className="picker__axis" aria-hidden="true" />
      </div>
      <div className="picker__legend">
        <span>
          <i style={{ background: META.AC.color }} /> AC cheaper
        </span>
        <span>
          <i style={{ background: META.TWAP.color }} /> TWAP cheaper
        </span>
        <span className="picker__wins">
          AC wins {wins} of {advantage.length}
        </span>
      </div>
      <div className="picker__jump" role="group" aria-label="Jump to a window">
        <button type="button" onClick={() => pickIdx(median)}>
          Median window
        </button>
        <button type="button" onClick={() => pickIdx(best)}>
          AC&apos;s best
        </button>
        <button type="button" onClick={() => pickIdx(worst)}>
          AC&apos;s worst
        </button>
      </div>
      <p className="picker__now">
        Replaying <b>{clockHm(openSec + selected * 60)}</b> to{' '}
        <b>{clockHm(openSec + (selected + minutes) * 60)}</b> ET
      </p>
    </div>
  );
}
export const Picker = memo(PickerImpl);
