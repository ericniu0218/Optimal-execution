import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { App } from './App';
import { installStrategyColors } from './lib/strategies';
import './styles.css';

installStrategyColors();
// `?og` is the capture mode used to render the social-share card (it hides the live-timing tile,
// which has no meaning under a frozen headless clock).
if (new URLSearchParams(window.location.search).has('og')) document.documentElement.classList.add('og');

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <App />
  </StrictMode>,
);
