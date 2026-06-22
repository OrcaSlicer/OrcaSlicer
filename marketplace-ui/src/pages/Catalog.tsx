import { useMemo, useState } from 'react';
import { useCatalog, useInstalled } from '../api/queries';
import { Card } from '../components/ui/Card';
import { Chip } from '../components/ui/Chip';
import { Toolbar, type ToolbarFilters } from '../components/Toolbar';
import { hrefFor } from '../hooks/useHashRoute';
import type { InstalledPluginInfo, MarketplacePlugin } from '../types';

function compareSemver(a: string, b: string): number {
  const pa = a.split('.').map((x) => Number.parseInt(x, 10));
  const pb = b.split('.').map((x) => Number.parseInt(x, 10));
  const len = Math.max(pa.length, pb.length);
  for (let i = 0; i < len; i++) {
    const x = pa[i] ?? 0;
    const y = pb[i] ?? 0;
    if (Number.isNaN(x) || Number.isNaN(y)) {
      return a.localeCompare(b);
    }
    if (x !== y) return x - y;
  }
  return 0;
}

function statusFor(
  plugin: MarketplacePlugin,
  installed: InstalledPluginInfo | undefined,
): { label: string; tone: 'neutral' | 'accent' | 'success' | 'warning' } {
  if (!installed) return { label: 'Install', tone: 'accent' };
  const cmp = compareSemver(plugin.latest_version, installed.version);
  if (cmp > 0) return { label: `Update to ${plugin.latest_version}`, tone: 'warning' };
  return { label: 'Installed', tone: 'success' };
}

function mostRecentTimestamp(plugin: MarketplacePlugin): number {
  let max = 0;
  for (const v of plugin.versions) {
    const t = Date.parse(v.published_at);
    if (!Number.isNaN(t) && t > max) max = t;
  }
  return max;
}

const DEFAULT_FILTERS: ToolbarFilters = {
  search: '',
  category: 'all',
  kind: 'all',
  sort: 'name',
};

export function CatalogPage() {
  const catalogQuery = useCatalog();
  const installedQuery = useInstalled();
  const [filters, setFilters] = useState<ToolbarFilters>(DEFAULT_FILTERS);

  const installedById = useMemo(() => {
    const m = new Map<string, InstalledPluginInfo>();
    for (const p of installedQuery.data?.plugins ?? []) m.set(p.id, p);
    return m;
  }, [installedQuery.data]);

  const filtered = useMemo(() => {
    const plugins = catalogQuery.data?.plugins ?? [];
    const needle = filters.search.trim().toLowerCase();

    const result = plugins.filter((p) => {
      if (filters.category !== 'all' && p.category !== filters.category) return false;
      if (filters.kind !== 'all') {
        const latest = p.versions.find((v) => v.version === p.latest_version) ?? p.versions[0];
        if (!latest || latest.kind !== filters.kind) return false;
      }
      if (needle) {
        const hay = [p.name, p.description, ...p.tags].join(' ').toLowerCase();
        if (!hay.includes(needle)) return false;
      }
      return true;
    });

    if (filters.sort === 'name') {
      result.sort((a, b) => a.name.localeCompare(b.name));
    } else {
      result.sort((a, b) => mostRecentTimestamp(b) - mostRecentTimestamp(a));
    }
    return result;
  }, [catalogQuery.data, filters]);

  if (catalogQuery.isLoading) {
    return <p className="text-orca-muted text-sm">Loading catalog...</p>;
  }
  if (catalogQuery.isError) {
    return (
      <Card className="p-4 border-orca-danger/40">
        <p className="text-orca-danger text-sm">
          Failed to load catalog: {String(catalogQuery.error)}
        </p>
      </Card>
    );
  }

  return (
    <div className="space-y-4">
      <Toolbar value={filters} onChange={setFilters} resultCount={filtered.length} />

      {filtered.length === 0 ? (
        <Card className="p-6 text-center text-sm text-orca-muted">
          No plugins match the current filters.
        </Card>
      ) : (
        <div className="grid gap-3">
          {filtered.map((plugin) => {
            const installed = installedById.get(plugin.id);
            const status = statusFor(plugin, installed);
            const latest = plugin.versions.find((v) => v.version === plugin.latest_version)
              ?? plugin.versions[0];
            return (
              <a
                key={plugin.id}
                href={hrefFor({ kind: 'detail', id: plugin.id })}
                className="block focus:outline-none focus-visible:ring-2 focus-visible:ring-orca-accent rounded-lg"
              >
                <Card className="p-4 hover:border-orca-muted transition-colors">
                  <div className="flex items-start gap-4">
                    <div className="flex-1 min-w-0">
                      <div className="flex items-center gap-2 flex-wrap">
                        <h3 className="text-base font-semibold text-orca-text truncate">
                          {plugin.name}
                        </h3>
                        <Chip tone="neutral">{plugin.category}</Chip>
                        {latest ? <Chip tone="accent">{latest.kind}</Chip> : null}
                      </div>
                      <p className="text-xs text-orca-muted mt-0.5">
                        by {plugin.author} &middot; v{plugin.latest_version}
                      </p>
                      <p className="text-sm text-orca-text/90 mt-2 line-clamp-2">
                        {plugin.description}
                      </p>
                      {plugin.tags.length > 0 ? (
                        <div className="flex flex-wrap gap-1.5 mt-3">
                          {plugin.tags.map((t) => (
                            <Chip key={t} tone="neutral">
                              {t}
                            </Chip>
                          ))}
                        </div>
                      ) : null}
                    </div>
                    <div className="shrink-0">
                      <Chip tone={status.tone}>{status.label}</Chip>
                    </div>
                  </div>
                </Card>
              </a>
            );
          })}
        </div>
      )}
    </div>
  );
}
