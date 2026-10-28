import { useState, useEffect } from 'preact/hooks';
import { StatusCard } from './components/StatusCard';
import { NetworkList } from './components/NetworkList';
import { SavedNetworks } from './components/SavedNetworks';
import { api } from './api/client';
import type { WifiStatus } from './types';
import './styles/variables.css';
import './styles/base.css';
import './styles/utilities.css';

export function App() {
  const [status, setStatus] = useState<WifiStatus | null>(null);
  const [loading, setLoading] = useState(true);
  // bumped when the network list may have changed elsewhere (e.g. connecting from the scan)
  const [savedVersion, setSavedVersion] = useState(0);

  const loadStatus = async () => {
    try {
      const s = await api.getStatus();
      setStatus(s);
    } catch (e) {
      console.error('Failed to load status:', e);
    } finally {
      setLoading(false);
    }
  };

  useEffect(() => {
    loadStatus();
    const interval = setInterval(loadStatus, 5000);
    return () => clearInterval(interval);
  }, []);

  const handleConnect = () => {
    setSavedVersion((v) => v + 1);
    setTimeout(loadStatus, 2000);
  };

  return (
    <div class="app">
      <header class="header">
        <h1>Nevermore · Wi-Fi</h1>
      </header>

      <StatusCard status={status} loading={loading} />
      <NetworkList onConnect={handleConnect} />
      <SavedNetworks version={savedVersion} />
    </div>
  );
}
