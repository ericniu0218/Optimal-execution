import { useCallback, useEffect, useRef } from 'react';

export interface CanvasSurface {
  ctx: CanvasRenderingContext2D;
  /** CSS pixel size of the drawing area. */
  w: number;
  h: number;
  dpr: number;
}

/**
 * Size a <canvas> to its CSS box at the device pixel ratio and keep it sharp on
 * resize. `draw` is read through a ref so a fresh closure never re-attaches the
 * observer; call the returned `redraw` (or just re-render) to repaint.
 */
export function useCanvas(draw: (s: CanvasSurface) => void) {
  const ref = useRef<HTMLCanvasElement>(null);
  const drawRef = useRef(draw);
  drawRef.current = draw;

  const paint = useCallback(() => {
    const canvas = ref.current;
    if (!canvas) return;
    const rect = canvas.getBoundingClientRect();
    const dpr = Math.min(window.devicePixelRatio || 1, 2.5);
    const w = Math.max(1, Math.round(rect.width));
    const h = Math.max(1, Math.round(rect.height));
    if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
      canvas.width = Math.round(w * dpr);
      canvas.height = Math.round(h * dpr);
    }
    const ctx = canvas.getContext('2d');
    if (!ctx) return;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    drawRef.current({ ctx, w, h, dpr });
  }, []);

  useEffect(() => {
    const canvas = ref.current;
    if (!canvas) return;
    const ro = new ResizeObserver(() => paint());
    ro.observe(canvas);
    return () => ro.disconnect();
  }, [paint]);

  // Repaint after every render: the draw closure captures the latest props.
  useEffect(() => {
    paint();
  });

  return { ref, paint };
}
