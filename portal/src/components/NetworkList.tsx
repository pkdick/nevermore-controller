import { useState } from 'preact/hooks';
import { Card } from './ui/Card';
import { Button } from './ui/Button';
import { SignalBars, rssiToQuality } from './ui/SignalBars';
import type { ScanResult, WifiStatus } from '../types';
import { api } from '../api/client';
import './NetworkList.css';

interface Props {
  onConnect: (ssid: string) => void;
  status: WifiStatus | null;
}

export function NetworkList({ onConnect, status }: Props) {
  const [networks, setNetworks] = useState<ScanResult[]>([]);
  const [saved, setSaved] = useState<Set<string>>(new Set());
  const [scanning, setScanning] = useState(false);
  const [scanned, setScanned] = useState(false);
  const [selectedSsid, setSelectedSsid] = useState<string | null>(null);
  const [password, setPassword] = useState('');
  const [connecting, setConnecting] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const scan = async () => {
    setScanning(true);
    setError(null);
    try {
      const results = await api.scan();
      setNetworks([...results].sort((a, b) => b.rssi - a.rssi));
      await loadSaved();
      setScanned(true);
    } catch (e) {
      setError(`Recherche impossible : ${(e as Error).message}`);
    } finally {
      setScanning(false);
    }
  };

  const loadSaved = async () => {
    try {
      setSaved(new Set((await api.getNetworks()).map((n) => n.ssid)));
    } catch {
      // only cosmetic
    }
  };

  const connect = async (ssid: string) => {
    setConnecting(true);
    setError(null);
    let added = false;
    try {
      // Add network first if password provided
      if (password) {
        await api.addNetwork(ssid, password);
        added = true;
      }
      await api.connect(ssid);
    } catch (e) {
      // `fetch` itself failing (`TypeError`) after the network was saved: the hotspot is likely
      // switching channel to join it. The status card will tell the outcome.
      if (!(added && e instanceof TypeError)) {
        setError(`Connexion impossible : ${(e as Error).message}`);
        setConnecting(false);
        return;
      }
    }
    setConnecting(false);
    onConnect(ssid);
    setSaved((s) => new Set(s).add(ssid));
    setSelectedSsid(null);
    setPassword('');
  };

  const isCurrent = (ssid: string) => status?.state === 'connected' && status.ssid === ssid;

  return (
    <Card
      title="Réseaux disponibles"
      action={
        <Button size="sm" variant="secondary" onClick={scan} loading={scanning}>
          Rechercher
        </Button>
      }
    >
      {error && <div class="network-error text-sm">{error}</div>}
      {networks.length === 0 ? (
        <div class="network-empty">
          {scanning
            ? 'Recherche en cours…'
            : scanned
              ? 'Aucun réseau trouvé'
              : 'Appuie sur « Rechercher » pour lister les réseaux'}
        </div>
      ) : (
        <div class="network-list">
          {networks.map((net) => (
            <div class="network-item" key={net.ssid}>
              <SignalBars quality={rssiToQuality(net.rssi)} />
              <div class="network-info">
                <div class="network-ssid">
                  {net.auth !== 'OPEN' && <span class="lock">🔒</span>}
                  {net.ssid || '(masqué)'}
                  {isCurrent(net.ssid)
                    ? <span class="network-badge network-badge-current">Connecté</span>
                    : saved.has(net.ssid) && <span class="network-badge">Mémorisé</span>}
                </div>
                <div class="network-meta text-sm text-muted" title={`${net.rssi} dBm`}>
                  Signal {rssiToQuality(net.rssi)} % • {net.auth === 'OPEN' ? 'ouvert' : net.auth}
                </div>
              </div>
              <Button
                size="sm"
                onClick={() => {
                  setSelectedSsid(selectedSsid === net.ssid ? null : net.ssid);
                  setPassword('');
                }}
              >
                {selectedSsid === net.ssid ? 'Annuler' : 'Choisir'}
              </Button>

              {selectedSsid === net.ssid && (
                <div class="network-connect-form">
                  {net.auth !== 'OPEN' && (
                    <input
                      type="password"
                      placeholder="Mot de passe"
                      autocomplete="off"
                      value={password}
                      onInput={(e) => setPassword((e.target as HTMLInputElement).value)}
                    />
                  )}
                  <Button
                    onClick={() => connect(net.ssid)}
                    loading={connecting}
                    class="w-full"
                  >
                    Mémoriser et se connecter
                  </Button>
                </div>
              )}
            </div>
          ))}
        </div>
      )}
    </Card>
  );
}
