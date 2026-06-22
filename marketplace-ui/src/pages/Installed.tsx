import { useMemo, useState } from 'react';
import { useCatalog, useInstalled, useUninstall, useUpdatePlugin } from '../api/queries';
import { Button } from '../components/ui/Button';
import { Card } from '../components/ui/Card';
import { Chip } from '../components/ui/Chip';
import { Modal } from '../components/ui/Modal';
import { PermissionList } from '../components/PermissionList';
import { hrefFor } from '../hooks/useHashRoute';
import type { InstalledPluginInfo, MarketplaceCatalog } from '../types';

function compareSemver(a: string, b: string): number {
  const pa = a.split('.').map((x) => Number.parseInt(x, 10));
  const pb = b.split('.').map((x) => Number.parseInt(x, 10));
  const len = Math.max(pa.length, pb.length);
  for (let i = 0; i < len; i++) {
    const x = pa[i] ?? 0;
    const y = pb[i] ?? 0;
    if (Number.isNaN(x) || Number.isNaN(y)) return a.localeCompare(b);
    if (x !== y) return x - y;
  }
  return 0;
}

function findUpdate(
  catalog: MarketplaceCatalog | undefined,
  installed: InstalledPluginInfo,
): string | null {
  const plugin = catalog?.plugins.find((p) => p.id === installed.id);
  if (!plugin) return null;
  return compareSemver(plugin.latest_version, installed.version) > 0
    ? plugin.latest_version
    : null;
}

type PendingDialog =
  | { kind: 'update'; plugin: InstalledPluginInfo; targetVersion: string }
  | { kind: 'uninstall'; plugin: InstalledPluginInfo };

export function InstalledPage() {
  const installedQuery = useInstalled();
  const catalogQuery = useCatalog();
  const updateMutation = useUpdatePlugin();
  const uninstallMutation = useUninstall();

  const [pending, setPending] = useState<PendingDialog | null>(null);
  const [resultMessage, setResultMessage] = useState<string | null>(null);

  const rows = useMemo(() => {
    const list = installedQuery.data?.plugins ?? [];
    return list.map((p) => ({ plugin: p, update: findUpdate(catalogQuery.data, p) }));
  }, [installedQuery.data, catalogQuery.data]);

  if (installedQuery.isLoading) {
    return <p className="text-orca-muted text-sm">Loading installed plugins...</p>;
  }
  if (installedQuery.isError) {
    return (
      <Card className="p-4 border-orca-danger/40">
        <p className="text-orca-danger text-sm">
          Failed to load installed plugins: {String(installedQuery.error)}
        </p>
      </Card>
    );
  }

  async function confirmAction() {
    if (!pending) return;
    setResultMessage(null);
    try {
      if (pending.kind === 'update') {
        const res = await updateMutation.mutateAsync({
          id: pending.plugin.id,
          version: pending.targetVersion,
        });
        if (res.status === 'ok') setPending(null);
        else setResultMessage(res.message ?? `Update ${res.status}.`);
      } else {
        const res = await uninstallMutation.mutateAsync(pending.plugin.id);
        if (res.status === 'ok') setPending(null);
        else setResultMessage(res.message ?? `Uninstall ${res.status}.`);
      }
    } catch (err) {
      setResultMessage(err instanceof Error ? err.message : String(err));
    }
  }

  const isBusy = updateMutation.isPending || uninstallMutation.isPending;

  return (
    <div className="space-y-4">
      <div className="flex items-center justify-between">
        <h2 className="text-base font-semibold">Installed plugins</h2>
        <span className="text-xs text-orca-muted">
          {rows.length} {rows.length === 1 ? 'plugin' : 'plugins'}
        </span>
      </div>

      {rows.length === 0 ? (
        <Card className="p-6 text-center text-sm text-orca-muted">
          No plugins installed yet.{' '}
          <a href={hrefFor({ kind: 'catalog' })} className="underline">
            Browse the catalog
          </a>{' '}
          to find something to add.
        </Card>
      ) : (
        <div className="grid gap-3">
          {rows.map(({ plugin, update }) => (
            <Card key={plugin.id} className="p-4">
              <div className="flex items-start gap-4 flex-wrap">
                <div className="flex-1 min-w-0">
                  <div className="flex items-center gap-2 flex-wrap">
                    <a
                      href={hrefFor({ kind: 'detail', id: plugin.id })}
                      className="text-base font-semibold text-orca-text hover:underline truncate"
                    >
                      {plugin.name}
                    </a>
                    <Chip tone="neutral">v{plugin.version}</Chip>
                    {update ? <Chip tone="warning">update available: v{update}</Chip> : null}
                  </div>
                  <p className="text-xs text-orca-muted mt-0.5">by {plugin.author}</p>
                  <p className="text-sm text-orca-text/90 mt-2 line-clamp-2">
                    {plugin.description}
                  </p>
                  <div className="mt-3">
                    <PermissionList permissions={plugin.permissions} compact />
                  </div>
                </div>
                <div className="flex gap-2 shrink-0">
                  {update ? (
                    <Button
                      onClick={() =>
                        setPending({ kind: 'update', plugin, targetVersion: update })
                      }
                      disabled={isBusy}
                    >
                      Update to v{update}
                    </Button>
                  ) : (
                    <Button variant="secondary" size="md" disabled>
                      Up to date
                    </Button>
                  )}
                  <Button
                    variant="danger"
                    onClick={() => setPending({ kind: 'uninstall', plugin })}
                    disabled={isBusy}
                  >
                    Uninstall
                  </Button>
                </div>
              </div>
            </Card>
          ))}
        </div>
      )}

      <Modal
        open={pending !== null}
        title={
          pending?.kind === 'update'
            ? `Update ${pending.plugin.name} to v${pending.targetVersion}`
            : pending?.kind === 'uninstall'
              ? `Uninstall ${pending.plugin.name}`
              : ''
        }
        onClose={() => {
          if (!isBusy) setPending(null);
        }}
        footer={
          <>
            <Button variant="secondary" onClick={() => setPending(null)} disabled={isBusy}>
              Cancel
            </Button>
            <Button
              variant={pending?.kind === 'uninstall' ? 'danger' : 'primary'}
              onClick={confirmAction}
              disabled={isBusy}
            >
              {isBusy ? 'Working...' : 'Confirm'}
            </Button>
          </>
        }
      >
        {pending?.kind === 'uninstall' ? (
          <p className="text-sm">
            Remove <strong>{pending.plugin.name}</strong>? Plugin data in its sandbox directory
            will be deleted.
          </p>
        ) : pending?.kind === 'update' ? (
          <p className="text-sm">
            The latest version will be downloaded, verified, and replace v{pending.plugin.version}.
          </p>
        ) : null}
        {resultMessage ? (
          <p className="mt-3 text-sm text-orca-danger">{resultMessage}</p>
        ) : null}
      </Modal>
    </div>
  );
}
