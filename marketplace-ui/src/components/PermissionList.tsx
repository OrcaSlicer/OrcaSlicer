import { Chip } from './ui/Chip';

const PERMISSION_HELP: Record<string, string> = {
  network: 'Outbound HTTP/HTTPS to any host',
  'network-restricted': 'Outbound HTTP/HTTPS to an allow-list of hosts',
  'filesystem-read': 'Read files inside the plugin sandbox directory',
  'filesystem-write': 'Read and write files inside the plugin sandbox directory',
  'engine-events': 'Subscribe to OrcaSlicer engine events (slice progress, exports, etc.)',
  'engine-config': 'Read and write OrcaSlicer print/printer/filament configuration',
  'device-bridge': 'Open USB/serial connections to attached printers',
  'webview-ui': 'Render an embedded webview panel inside OrcaSlicer',
  clipboard: 'Read from and write to the system clipboard',
  notifications: 'Show desktop notifications',
};

function toneForPermission(perm: string): 'neutral' | 'warning' | 'danger' {
  if (perm === 'network' || perm === 'device-bridge') return 'danger';
  if (perm.startsWith('filesystem') || perm === 'engine-config') return 'warning';
  return 'neutral';
}

interface PermissionListProps {
  permissions: string[];
  compact?: boolean;
}

export function PermissionList({ permissions, compact = false }: PermissionListProps) {
  if (permissions.length === 0) {
    return <p className="text-xs text-orca-muted italic">No special permissions required.</p>;
  }

  if (compact) {
    return (
      <div className="flex flex-wrap gap-1.5">
        {permissions.map((p) => (
          <Chip key={p} tone={toneForPermission(p)} title={PERMISSION_HELP[p] ?? p}>
            {p}
          </Chip>
        ))}
      </div>
    );
  }

  return (
    <ul className="space-y-2">
      {permissions.map((p) => (
        <li key={p} className="flex items-start gap-3">
          <Chip tone={toneForPermission(p)} className="mt-0.5">
            {p}
          </Chip>
          <span className="text-xs text-orca-muted leading-relaxed">
            {PERMISSION_HELP[p] ?? 'Custom permission scope defined by the plugin.'}
          </span>
        </li>
      ))}
    </ul>
  );
}
