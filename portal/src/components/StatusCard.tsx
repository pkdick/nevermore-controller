import { Card } from './ui/Card';
import { Button } from './ui/Button';
import { SignalBars } from './ui/SignalBars';
import type { WifiStatus } from '../types';
import './StatusCard.css';

// A connection we asked for & whose outcome isn't known yet.
export interface Pending {
  ssid: string;
  since: number;  // `Date.now()`
  seenConnecting: boolean;
}

interface Props {
  status: WifiStatus | null;
  loading: boolean;
  lost: boolean;
  pending: Pending | null;
  // connected while on the hotspot (kept after the hotspot closed)
  success: WifiStatus | null;
  apSsid: string;
  onDismiss: () => void;
}

// A failure state may be left over from before our request; trust it once the attempt started.
const PENDING_TRUST_FAILURE_AFTER_MS = 15000;

function failureText(status: WifiStatus): string | null {
  switch (status.state) {
    case 'wrong_password': return `« ${status.ssid} » a refusé le mot de passe.`;
    case 'not_found': return `« ${status.ssid} » est introuvable (hors de portée ?).`;
    case 'failed': return `La connexion à « ${status.ssid} » a échoué.`;
    default: return null;
  }
}

const WifiIcon = () => (
  <svg viewBox="0 0 24 24" fill="currentColor">
    <path d="M1 9l2 2c4.97-4.97 13.03-4.97 18 0l2-2C16.93 2.93 7.08 2.93 1 9zm8 8l3 3 3-3c-1.65-1.66-4.34-1.66-6 0zm-4-4l2 2c2.76-2.76 7.24-2.76 10 0l2-2C15.14 9.14 8.87 9.14 5 13z"/>
  </svg>
);

const WifiOffIcon = () => (
  <svg viewBox="0 0 24 24" fill="currentColor">
    <path d="M23.64 7c-.45-.34-4.93-4-11.64-4-1.5 0-2.89.19-4.15.48L18.18 13.8 23.64 7zm-6.6 8.22L3.27 1.44 2 2.72l2.05 2.06C1.91 5.76.59 6.82.36 7l11.63 14.49.01.01.01-.01 3.9-4.86 3.32 3.32 1.27-1.27-3.46-3.46z"/>
  </svg>
);

export function StatusCard({ status, loading, lost, pending, success, apSsid, onDismiss }: Props) {
  if (loading) {
    return (
      <Card>
        <div class="status-loading">Chargement…</div>
      </Card>
    );
  }

  const hotspot = apSsid ? `« ${apSsid} »` : 'le hotspot du filtre';

  if (success) {
    return (
      <Card>
        <div class="status-banner status-banner-success">
          <div class="status-content">
            <div class="status-icon connected"><WifiIcon /></div>
            <div class="status-info">
              <div class="status-state">Connexion réussie</div>
              <div>Le filtre est connecté à « {success.ssid} »</div>
              <div class="status-ip">{success.ip} · {success.hostname}.local</div>
              <div class="status-signal">
                <SignalBars quality={success.quality} />
                <span class="text-sm text-muted">{success.quality} %</span>
              </div>
            </div>
          </div>
          <div class="text-sm status-hint">
            C'est terminé. Le hotspot {hotspot} va se fermer dans quelques secondes et ton
            téléphone reviendra sur ton réseau habituel. Depuis ce réseau, l'état du filtre et
            ses capteurs sont visibles sur http://{success.hostname}.local/
          </div>
        </div>
      </Card>
    );
  }

  if (pending) {
    const failure =
      status && !lost && status.ssid === pending.ssid &&
      (pending.seenConnecting || PENDING_TRUST_FAILURE_AFTER_MS < Date.now() - pending.since)
        ? failureText(status)
        : null;

    if (failure) {
      return (
        <Card>
          <div class="status-banner status-banner-error">
            <div class="status-state">{failure}</div>
            <div class="text-sm">
              Le réseau reste mémorisé et le filtre réessaiera tout seul. Pour corriger le mot de
              passe, choisis à nouveau le réseau ci-dessous.
            </div>
            <Button size="sm" variant="secondary" onClick={onDismiss}>OK</Button>
          </div>
        </Card>
      );
    }

    return (
      <Card>
        <div class="status-content">
          <div class="status-icon pending"><WifiIcon /></div>
          <div class="status-info">
            <div class="status-state">Connexion à « {pending.ssid} »…</div>
            <div class="text-sm text-muted status-hint">
              {lost
                ? <>Liaison avec le filtre interrompue, c'est normal : il change de canal pour
                    rejoindre « {pending.ssid} ». Si ton téléphone ne revient pas tout seul sur
                    {' '}{hotspot} d'ici une minute, reconnecte-le : le résultat s'affichera ici.</>
                : <>Le hotspot peut se couper quelques secondes pendant que le filtre change de
                    canal. Reste sur cette page.</>}
            </div>
          </div>
        </div>
      </Card>
    );
  }

  if (!status) {
    return (
      <Card>
        <div class="status-error">Impossible de lire l'état du filtre</div>
      </Card>
    );
  }

  const isConnected = status.state === 'connected';
  const isConnecting = status.state === 'connecting';
  const failure = failureText(status);

  return (
    <Card>
      <div class="status-content">
        <div class={`status-icon ${isConnected ? 'connected' : ''}`}>
          {isConnected ? <WifiIcon /> : <WifiOffIcon />}
        </div>
        <div class="status-info">
          <div class="status-state">
            {isConnected
              ? `Connecté à « ${status.ssid} »`
              : isConnecting
                ? `Connexion à « ${status.ssid} »…`
                : failure ?? 'Non connecté'}
          </div>
          {isConnected && (
            <>
              <div class="status-ip">{status.ip}</div>
              <div class="status-signal">
                <SignalBars quality={status.quality} />
                <span class="text-sm text-muted">{status.quality} %</span>
              </div>
            </>
          )}
          {lost && (
            <div class="text-sm status-hint status-lost">
              Liaison avec le filtre perdue. Ton téléphone est-il toujours connecté à {hotspot} ?
            </div>
          )}
        </div>
      </div>
    </Card>
  );
}
