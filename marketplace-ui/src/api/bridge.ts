import type {
  ActionResult,
  InstalledResponse,
  MarketplaceCatalog,
  MarketplaceConfig,
  OrcaMarketplaceBridge,
} from '../types';

// Dev-mode fixture for `config()`. The native bridge fills in the real
// resolved URL at runtime; the fixture lets the SPA render against the
// configured-state path when run via `vite dev`. Override per-session by
// setting `VITE_DEV_MARKETPLACE_URL=""` to exercise the empty-state.
const DEV_MARKETPLACE_URL: string =
  (typeof import.meta !== 'undefined' &&
    typeof (import.meta as any).env !== 'undefined' &&
    typeof (import.meta as any).env.VITE_DEV_MARKETPLACE_URL === 'string')
    ? (import.meta as any).env.VITE_DEV_MARKETPLACE_URL
    : 'https://example.invalid/marketplace';

const FIXTURE_CATALOG: MarketplaceCatalog = {
  revision_sha256: 'dev0000000000000000000000000000000000000000000000000000000000fixt',
  generated_at: '2026-06-03T12:00:00Z',
  plugins: [
    {
      id: 'community.example-device-agent',
      name: 'Example Device Agent',
      description:
        'Sample device-agent plugin entry: webview dashboard + engine event integration. Demonstrates the hybrid plugin kind with a device-agent slot.',
      category: 'device',
      tags: ['device-agent', 'webview', 'sample'],
      author: 'community',
      homepage: 'https://example.invalid/device-agent',
      repo: 'https://example.invalid/community/example-device-agent',
      license: 'AGPL-3.0',
      screenshots: [],
      latest_version: '0.4.1',
      versions: [
        {
          version: '0.4.1',
          kind: 'hybrid',
          provides: ['device-agent', 'webview-dashboard'],
          permissions: ['network', 'filesystem-read', 'filesystem-write', 'engine-events'],
          engine_compat: '>=0.6.0',
          url: 'https://example.invalid/example-device-agent-0.4.1.orcaplug',
          sha256: 'aa11bb22cc33dd44ee55ff6677889900aabbccddeeff00112233445566778899',
          size_bytes: 2_415_000,
          published_at: '2026-05-21T10:30:00Z',
        },
        {
          version: '0.4.0',
          kind: 'hybrid',
          provides: ['device-agent', 'webview-dashboard'],
          permissions: ['network', 'filesystem-read', 'filesystem-write', 'engine-events'],
          engine_compat: '>=0.6.0',
          url: 'https://example.invalid/example-device-agent-0.4.0.orcaplug',
          sha256: '5555aaaa6666bbbb7777cccc8888dddd9999eeee0000ffff1111222233334444',
          size_bytes: 2_380_000,
          published_at: '2026-04-10T09:00:00Z',
        },
      ],
    },
    {
      id: 'community.voron-profile-pack',
      name: 'Voron Community Profile Pack',
      description:
        'Curated print, filament, and machine profiles for Voron 0/2.4/Trident builds. Tuned for ABS, ASA, PETG, and PA-CF. Maintained by the Voron community discord.',
      category: 'profile_pack',
      tags: ['voron', 'profiles', 'abs', 'asa', 'petg'],
      author: 'Voron Community',
      homepage: 'https://example.invalid/voron-pack',
      license: 'CC-BY-SA-4.0',
      screenshots: [],
      latest_version: '2026.05',
      versions: [
        {
          version: '2026.05',
          kind: 'data',
          provides: ['profile-pack'],
          permissions: ['filesystem-read'],
          engine_compat: '>=0.5.0',
          url: 'https://example.invalid/voron-pack-2026.05.orcaplug',
          sha256: 'fa11ce0000111122223333444455556666777788889999aaaabbbbccccddddee',
          size_bytes: 845_000,
          published_at: '2026-05-15T17:45:00Z',
        },
      ],
    },
    {
      id: 'community.seam-jitter',
      name: 'Seam Jitter G-code Filter',
      description:
        'Synthetic G-code post-processor that randomizes seam start positions within a configurable angular window to break the visible Z-seam line. Drop-in, no profile changes required.',
      category: 'gcode',
      tags: ['post-processor', 'seam', 'quality'],
      author: 'community',
      repo: 'https://example.invalid/community/seam-jitter',
      license: 'MIT',
      screenshots: [],
      latest_version: '1.2.0',
      versions: [
        {
          version: '1.2.0',
          kind: 'wasm',
          provides: ['gcode-filter'],
          permissions: ['engine-events'],
          engine_compat: '>=0.5.0',
          url: 'https://example.invalid/seam-jitter-1.2.0.orcaplug',
          sha256: 'beadface0000111122223333444455556666777788889999aaaabbbbccccdddd',
          size_bytes: 64_000,
          published_at: '2026-03-02T08:15:00Z',
        },
      ],
    },
  ],
};

const FIXTURE_INSTALLED: InstalledResponse = {
  plugins: [
    {
      id: 'community.example-device-agent',
      name: 'Example Device Agent',
      version: '0.4.0',
      author: 'community',
      description: 'Sample device-agent plugin.',
      permissions: ['network', 'filesystem-read', 'filesystem-write', 'engine-events'],
    },
  ],
};

function getNativeBridge(): OrcaMarketplaceBridge | undefined {
  if (typeof window === 'undefined') return undefined;
  return window.orca?.marketplace;
}

function delay<T>(value: T, ms = 120): Promise<T> {
  return new Promise((resolve) => setTimeout(() => resolve(value), ms));
}

function clone<T>(value: T): T {
  return JSON.parse(JSON.stringify(value)) as T;
}

const devState: { installed: InstalledResponse } = {
  installed: clone(FIXTURE_INSTALLED),
};

export const bridge: OrcaMarketplaceBridge = {
  async config(): Promise<MarketplaceConfig> {
    const native = getNativeBridge();
    if (native) return native.config();
    return delay({ registry_url: DEV_MARKETPLACE_URL, user_overridable: true });
  },

  async list(): Promise<MarketplaceCatalog> {
    const native = getNativeBridge();
    if (native) return native.list();
    return delay(clone(FIXTURE_CATALOG));
  },

  async installed(): Promise<InstalledResponse> {
    const native = getNativeBridge();
    if (native) return native.installed();
    return delay(clone(devState.installed));
  },

  async install(id: string, version: string): Promise<ActionResult> {
    const native = getNativeBridge();
    if (native) return native.install(id, version);
    const plugin = FIXTURE_CATALOG.plugins.find((p) => p.id === id);
    if (!plugin) {
      return delay<ActionResult>({ status: 'error', message: `Unknown plugin: ${id}` });
    }
    const existing = devState.installed.plugins.find((p) => p.id === id);
    const entry = {
      id: plugin.id,
      name: plugin.name,
      version,
      author: plugin.author,
      description: plugin.description,
      permissions: plugin.versions.find((v) => v.version === version)?.permissions ?? [],
    };
    if (existing) {
      Object.assign(existing, entry);
    } else {
      devState.installed.plugins.push(entry);
    }
    return delay<ActionResult>({ status: 'ok' });
  },

  async update(id: string, version: string): Promise<ActionResult> {
    return this.install(id, version);
  },

  async uninstall(id: string): Promise<ActionResult> {
    const native = getNativeBridge();
    if (native) return native.uninstall(id);
    devState.installed.plugins = devState.installed.plugins.filter((p) => p.id !== id);
    return delay<ActionResult>({ status: 'ok' });
  },

  async open_external(url: string): Promise<void> {
    const native = getNativeBridge();
    if (native) {
      await native.open_external(url);
      return;
    }
    if (typeof window !== 'undefined' && typeof window.open === 'function') {
      window.open(url, '_blank', 'noopener,noreferrer');
    }
  },
};

export function isNativeBridgeAvailable(): boolean {
  return getNativeBridge() !== undefined;
}
