import type { MarketplaceVersion } from '../types';
import { Chip } from './ui/Chip';

function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
}

function formatDate(rfc3339: string): string {
  try {
    const d = new Date(rfc3339);
    if (Number.isNaN(d.getTime())) return rfc3339;
    return d.toISOString().slice(0, 10);
  } catch {
    return rfc3339;
  }
}

interface VersionTableProps {
  versions: MarketplaceVersion[];
  installedVersion?: string | undefined;
}

export function VersionTable({ versions, installedVersion }: VersionTableProps) {
  return (
    <div className="overflow-x-auto border border-orca-border rounded-md">
      <table className="w-full text-sm">
        <thead className="bg-orca-bg text-orca-muted text-xs uppercase tracking-wide">
          <tr>
            <th className="text-left px-3 py-2">Version</th>
            <th className="text-left px-3 py-2">Kind</th>
            <th className="text-left px-3 py-2">Engine</th>
            <th className="text-left px-3 py-2">Size</th>
            <th className="text-left px-3 py-2">Published</th>
            <th className="text-left px-3 py-2">Status</th>
          </tr>
        </thead>
        <tbody>
          {versions.map((v) => {
            const isInstalled = installedVersion === v.version;
            return (
              <tr key={v.version} className="border-t border-orca-border">
                <td className="px-3 py-2 font-mono text-xs">{v.version}</td>
                <td className="px-3 py-2">
                  <Chip tone="accent">{v.kind}</Chip>
                </td>
                <td className="px-3 py-2 font-mono text-xs text-orca-muted">{v.engine_compat}</td>
                <td className="px-3 py-2 text-xs text-orca-muted">{formatBytes(v.size_bytes)}</td>
                <td className="px-3 py-2 text-xs text-orca-muted">{formatDate(v.published_at)}</td>
                <td className="px-3 py-2">
                  {isInstalled ? <Chip tone="success">installed</Chip> : null}
                </td>
              </tr>
            );
          })}
        </tbody>
      </table>
    </div>
  );
}
