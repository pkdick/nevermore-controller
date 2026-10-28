import { useState, useEffect } from 'preact/hooks';
import { Card } from './ui/Card';
import type { Sensors, SensorSide } from '../types';
import { api } from '../api/client';
import './SensorsCard.css';

const POLL_PERIOD_MS = 3000;

function fmt(x: number | null, digits: number, unit: string): string {
  return x === null ? '—' : `${x.toFixed(digits)} ${unit}`;
}

// Sensirion's VOC index: 100 = the average of the last 24 h, higher = more VOCs than usual.
function vocLabel(x: number | null): string {
  if (x === null) return '';
  if (x <= 80) return 'mieux que d’habitude';
  if (x <= 150) return 'habituel';
  if (x <= 250) return 'élevé';
  return 'très élevé';
}

function Side({ title, side }: { title: string; side: SensorSide }) {
  return (
    <div class="sensors-side">
      <div class="sensors-side-title">{title}</div>
      <dl>
        <dt>Température</dt>
        <dd>{fmt(side.temperature, 1, '°C')}</dd>
        <dt>Humidité</dt>
        <dd>{fmt(side.humidity, 0, '%')}</dd>
        <dt>Pression</dt>
        <dd>{fmt(side.pressure, 0, 'hPa')}</dd>
        <dt>Indice COV</dt>
        <dd>
          {fmt(side.voc_index, 0, '')}
          {side.voc_index !== null && <span class="text-sm text-muted"> {vocLabel(side.voc_index)}</span>}
        </dd>
      </dl>
    </div>
  );
}

export function SensorsCard() {
  const [sensors, setSensors] = useState<Sensors | null>(null);
  const [error, setError] = useState(false);

  useEffect(() => {
    const load = async () => {
      try {
        setSensors(await api.getSensors());
        setError(false);
      } catch {
        setError(true);
      }
    };
    load();
    const interval = setInterval(load, POLL_PERIOD_MS);
    return () => clearInterval(interval);
  }, []);

  if (!sensors) {
    return (
      <Card title="Filtre">
        <div class="status-loading">{error ? 'Impossible de lire les capteurs' : 'Chargement…'}</div>
      </Card>
    );
  }

  const fan = sensors.fan;
  return (
    <>
      <Card title="Ventilateur">
        <div class="fan">
          <div class="fan-value">{fan.power.toFixed(0)} %</div>
          <div class="fan-bar"><div class="fan-bar-fill" style={{ width: `${Math.min(100, fan.power)}%` }} /></div>
          <div class="text-sm text-muted">
            {fan.rpm.toFixed(0)} tr/min
            {fan.override !== null && <> • forcé à {fan.override.toFixed(0)} %</>}
          </div>
        </div>
      </Card>
      <Card title="Capteurs">
        <div class="sensors-grid">
          <Side title="Entrée" side={sensors.intake} />
          <Side title="Sortie" side={sensors.exhaust} />
        </div>
        <div class="text-sm text-muted sensors-footer">
          Processeur {fmt(sensors.mcu_temperature, 1, '°C')}
          {error && <span class="status-lost"> • liaison interrompue</span>}
        </div>
      </Card>
    </>
  );
}
