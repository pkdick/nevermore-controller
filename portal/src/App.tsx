import { useState, useEffect, useRef } from 'preact/hooks';
import { StatusCard, type Pending } from './components/StatusCard';
import { NetworkList } from './components/NetworkList';
import { SavedNetworks } from './components/SavedNetworks';
import { SensorsCard } from './components/SensorsCard';
import { api } from './api/client';
import type { WifiStatus } from './types';
import './styles/variables.css';
import './styles/base.css';
import './styles/utilities.css';

const POLL_PERIOD_MS = 5000;
const POLL_PERIOD_PENDING_MS = 2000;

export function App() {
  const [status, setStatus] = useState<WifiStatus | null>(null);
  const [loading, setLoading] = useState(true);
  // the last status request failed: e.g. the phone dropped off the hotspot
  const [lost, setLost] = useState(false);
  // a connection we asked for & whose outcome isn't known yet
  const [pending, setPending] = useState<Pending | null>(null);
  // Connected while on the hotspot: setup is done. Kept once seen, since the hotspot closes
  // a few seconds later (which loses the link).
  const [success, setSuccess] = useState<WifiStatus | null>(null);
  // bumped when the network list may have changed elsewhere (e.g. connecting from the scan)
  const [savedVersion, setSavedVersion] = useState(0);
  // the hotspot's name, kept for when the link is lost
  const apSsid = useRef('');

  const loadStatus = async () => {
    try {
      const s = await api.getStatus();
      if (s.ap_ssid) apSsid.current = s.ap_ssid;
      setStatus(s);
      setLost(false);
      if (!s.readonly && s.state === 'connected' && s.ap_active) setSuccess(s);
      setPending((p) => {
        if (!p || s.ssid !== p.ssid) return p;
        if (s.state === 'connecting') return { ...p, seenConnecting: true };
        // Joining a network on another channel moves the hotspot there too: the phone may
        // only come back once it's over, so `connected` can be the first state we see.
        if (s.state === 'connected') return null;
        return p;
      });
    } catch (e) {
      console.error('Failed to load status:', e);
      setLost(true);
    } finally {
      setLoading(false);
    }
  };

  useEffect(() => {
    loadStatus();
    const interval = setInterval(loadStatus, pending ? POLL_PERIOD_PENDING_MS : POLL_PERIOD_MS);
    return () => clearInterval(interval);
  }, [pending !== null]);

  useEffect(() => {
    document.title = status?.readonly ? 'Nevermore' : 'Nevermore · Wi-Fi';
  }, [status?.readonly]);

  const handleConnect = (ssid: string) => {
    setSavedVersion((v) => v + 1);
    setSuccess(null);
    setPending({ ssid, since: Date.now(), seenConnecting: false });
    setTimeout(loadStatus, 1000);
  };

  // Home network: read-only dashboard, configuration is only possible from the hotspot.
  if (status?.readonly) {
    return (
      <div class="app">
        <header class="header">
          <h1>Nevermore</h1>
        </header>

        <StatusCard status={status} loading={false} lost={lost} pending={null} success={null}
          apSsid={apSsid.current} onDismiss={() => {}} />
        <SensorsCard />
        <p class="text-sm text-muted">
          Lecture seule. Le Wi-Fi du filtre se règle depuis son hotspot, qui s'ouvre quand aucun
          réseau mémorisé n'est joignable.
        </p>
      </div>
    );
  }

  return (
    <div class="app">
      <header class="header">
        <h1>Nevermore · Wi-Fi</h1>
      </header>

      <StatusCard
        status={status}
        loading={loading}
        lost={lost}
        pending={pending}
        success={success}
        apSsid={apSsid.current}
        onDismiss={() => setPending(null)}
      />
      <NetworkList onConnect={handleConnect} status={status} />
      <SavedNetworks version={savedVersion} />
    </div>
  );
}
