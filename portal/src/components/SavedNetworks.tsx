import { useState, useEffect } from 'preact/hooks';
import { Card } from './ui/Card';
import { Button } from './ui/Button';
import type { SavedNetwork } from '../types';
import { api } from '../api/client';
import './SavedNetworks.css';

export function SavedNetworks({ version }: { version: number }) {
  const [networks, setNetworks] = useState<SavedNetwork[]>([]);
  const [loading, setLoading] = useState(true);
  const [showAdd, setShowAdd] = useState(false);
  const [ssid, setSsid] = useState('');
  const [password, setPassword] = useState('');
  const [adding, setAdding] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const load = async () => {
    try {
      const nets = await api.getNetworks();
      setNetworks(nets);
    } catch (e) {
      setError(`Lecture impossible : ${(e as Error).message}`);
    } finally {
      setLoading(false);
    }
  };

  useEffect(() => {
    load();
  }, [version]);

  const handleAdd = async () => {
    if (!ssid.trim()) return;
    setAdding(true);
    setError(null);
    try {
      await api.addNetwork(ssid, password);
      setSsid('');
      setPassword('');
      setShowAdd(false);
      load();
    } catch (e) {
      setError(`Ajout impossible : ${(e as Error).message}`);
    } finally {
      setAdding(false);
    }
  };

  const handleDelete = async (ssid: string) => {
    setError(null);
    try {
      await api.deleteNetwork(ssid);
      load();
    } catch (e) {
      setError(`Suppression impossible : ${(e as Error).message}`);
    }
  };

  return (
    <Card
      title="Réseaux mémorisés"
      action={
        <Button size="sm" variant="secondary" onClick={() => setShowAdd(!showAdd)}>
          {showAdd ? 'Annuler' : '+ Ajouter'}
        </Button>
      }
    >
      {error && <div class="network-error text-sm">{error}</div>}
      {showAdd && (
        <div class="add-network-form mb-4">
          <input
            type="text"
            placeholder="Nom du réseau (SSID)"
            value={ssid}
            onInput={(e) => setSsid((e.target as HTMLInputElement).value)}
          />
          <input
            type="password"
            placeholder="Mot de passe (vide si réseau ouvert)"
            autocomplete="off"
            value={password}
            onInput={(e) => setPassword((e.target as HTMLInputElement).value)}
          />
          <Button onClick={handleAdd} loading={adding} class="w-full">
            Mémoriser
          </Button>
        </div>
      )}

      {loading ? (
        <div class="saved-empty">Chargement…</div>
      ) : networks.length === 0 ? (
        <div class="saved-empty">Aucun réseau mémorisé</div>
      ) : (
        <div class="saved-list">
          {networks.map((net, i) => (
            <div class="saved-item" key={net.ssid}>
              <div class="saved-info">
                <div class="saved-ssid">{net.ssid}</div>
                <div class="text-sm text-muted">
                  {i === 0 ? 'Essayé en premier' : `Essayé en ${i + 1}ᵉ`}
                </div>
              </div>
              <Button size="sm" variant="ghost" onClick={() => handleDelete(net.ssid)}>
                ×
              </Button>
            </div>
          ))}
        </div>
      )}
    </Card>
  );
}
