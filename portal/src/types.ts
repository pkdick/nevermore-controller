export interface WifiStatus {
  // `wrong_password`, `not_found` & `failed`: the last attempt (`ssid`) failed, the filter keeps retrying
  state: 'connected' | 'connecting' | 'disconnected' | 'wrong_password' | 'not_found' | 'failed';
  ssid: string;
  rssi: number;
  quality: number;
  channel: number;
  ip: string;
  netmask: string;
  gateway: string;
  dns: string;
  mac: string;
  hostname: string;
  uptime_ms: number;
  ap_active: boolean;
  ap_ssid: string;
  // requested from the home network (not the hotspot): no configuration possible
  readonly: boolean;
}

// `null`: no sensor for it
export interface SensorSide {
  temperature: number | null;  // °C
  humidity: number | null;     // %
  pressure: number | null;     // hPa
  voc_index: number | null;    // [1, 500], 100 = average of the last 24 h
}

export interface Sensors {
  intake: SensorSide;
  exhaust: SensorSide;
  mcu_temperature: number | null;
  fan: {
    power: number;            // %
    rpm: number;
    override: number | null;  // %, set from the display or Klipper
  };
  uptime_s: number;
}

export interface ScanResult {
  ssid: string;
  rssi: number;
  auth: string;
}

export interface SavedNetwork {
  ssid: string;
  priority: number;
}

export interface APStatus {
  active: boolean;
  ssid: string;
  ip: string;
  channel: number;
  sta_count: number;
  clients: Array<{ mac: string; ip: string }>;
}

export interface APConfig {
  ssid: string;
  password: string;
  channel: number;
  max_connections: number;
  hidden: boolean;
  ip: string;
  netmask: string;
  gateway: string;
  dhcp_start: string;
  dhcp_end: string;
}

export interface Variable {
  key: string;
  value: string;
}
