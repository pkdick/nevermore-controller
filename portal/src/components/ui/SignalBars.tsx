import './SignalBars.css';

// Common RSSI -> quality mapping: -100 dBm = 0 %, -50 dBm = 100 %.
export function rssiToQuality(rssi: number) {
  return Math.max(0, Math.min(100, 2 * (rssi + 100)));
}

export function SignalBars({ quality }: { quality: number }) {
  const bars = 5;
  const filled = Math.round((quality / 100) * bars);
  return (
    <div class="signal-bars" title={`${quality} %`}>
      {Array.from({ length: bars }).map((_, i) => (
        <div class={`bar ${i < filled ? 'filled' : ''}`} />
      ))}
    </div>
  );
}
