import { useMemo, useState } from 'react';
import { bridge } from '../api/bridge';
import { useCatalog, useInstall, useInstalled, useUninstall, useUpdatePlugin } from '../api/queries';
import { Button } from '../components/ui/Button';
import { Card } from '../components/ui/Card';
import { Chip } from '../components/ui/Chip';
import { Modal } from '../components/ui/Modal';
import { PermissionList } from '../components/PermissionList';
import { VersionTable } from '../components/VersionTable';
import { hrefFor } from '../hooks/useHashRoute';
import type { MarketplacePlugin } from '../types';

interface PluginDetailPageProps {
  id: string;
}

interface ActionState {
  kind: 'install' | 'update' | 'uninstall';
  version: string;
}

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

function ScreenshotCarousel({ urls }: { urls: string[] }) {
  const [index, setIndex] = useState(0);
  if (urls.length === 0) {
    return (
      <div className="aspect-video bg-orca-bg border border-orca-border rounded-md flex items-center justify-center text-xs text-orca-muted">
        No screenshots provided
      </div>
    );
  }
  const safeIndex = Math.min(index, urls.length - 1);
  const current = urls[safeIndex] ?? urls[0]!;
  return (
    <div className="space-y-2">
      <div className="aspect-video bg-orca-bg border border-orca-border rounded-md overflow-hidden flex items-center justify-center">
        <img
          src={current}
          alt={`Screenshot ${safeIndex + 1}`}
          className="max-h-full max-w-full object-contain"
        />
      </div>
      {urls.length > 1 ? (
        <div className="flex gap-2 items-center justify-center">
          <Button variant="secondary" size="sm" onClick={() => setIndex((i) => Math.max(0, i - 1))}>
            Prev
          </Button>
          <span className="text-xs text-orca-muted">
            {safeIndex + 1} / {urls.length}
          </span>
          <Button
            variant="secondary"
            size="sm"
            onClick={() => setIndex((i) => Math.min(urls.length - 1, i + 1))}
          >
            Next
          </Button>
        </div>
      ) : null}
    </div>
  );
}

function PluginDetailBody({ plugin }: { plugin: MarketplacePlugin }) {
  const installedQuery = useInstalled();
  const installMutation = useInstall();
  const updateMutation = useUpdatePlugin();
  const uninstallMutation = useUninstall();

  const installed = installedQuery.data?.plugins.find((p) => p.id === plugin.id);
  const latestVersion = plugin.versions.find((v) => v.version === plugin.latest_version)
    ?? plugin.versions[0];

  const [pendingAction, setPendingAction] = useState<ActionState | null>(null);
  const [resultMessage, setResultMessage] = useState<string | null>(null);

  const sortedVersions = useMemo(
    () => [...plugin.versions].sort((a, b) => -compareSemver(a.version, b.version)),
    [plugin.versions],
  );

  if (!latestVersion) {
    return (
      <Card className="p-6 text-sm text-orca-muted">This plugin has no published versions.</Card>
    );
  }

  const canUpdate = installed !== undefined
    && compareSemver(plugin.latest_version, installed.version) > 0;

  function openInstall() {
    setResultMessage(null);
    setPendingAction({ kind: 'install', version: latestVersion!.version });
  }
  function openUpdate() {
    setResultMessage(null);
    setPendingAction({ kind: 'update', version: latestVersion!.version });
  }
  function openUninstall() {
    setResultMessage(null);
    setPendingAction({ kind: 'uninstall', version: installed?.version ?? '' });
  }

  async function confirmAction() {
    if (!pendingAction) return;
    setResultMessage(null);
    try {
      let res;
      if (pendingAction.kind === 'install') {
        res = await installMutation.mutateAsync({ id: plugin.id, version: pendingAction.version });
      } else if (pendingAction.kind === 'update') {
        res = await updateMutation.mutateAsync({ id: plugin.id, version: pendingAction.version });
      } else {
        res = await uninstallMutation.mutateAsync(plugin.id);
      }
      if (res.status === 'ok') {
        setPendingAction(null);
      } else {
        setResultMessage(res.message ?? `Action ${res.status}.`);
      }
    } catch (err) {
      setResultMessage(err instanceof Error ? err.message : String(err));
    }
  }

  function openExternal(url: string | undefined) {
    if (!url) return;
    void bridge.open_external(url);
  }

  const isBusy = installMutation.isPending
    || updateMutation.isPending
    || uninstallMutation.isPending;

  return (
    <div className="space-y-5">
      <div className="flex items-center justify-between">
        <a
          href={hrefFor({ kind: 'catalog' })}
          className="text-xs text-orca-muted hover:text-orca-text"
        >
          &larr; Back to catalog
        </a>
      </div>

      <Card className="p-5">
        <div className="flex items-start gap-4 flex-wrap">
          <div className="flex-1 min-w-0">
            <div className="flex items-center gap-2 flex-wrap">
              <h2 className="text-lg font-semibold">{plugin.name}</h2>
              <Chip tone="neutral">{plugin.category}</Chip>
              <Chip tone="accent">{latestVersion.kind}</Chip>
              {installed ? <Chip tone="success">installed v{installed.version}</Chip> : null}
            </div>
            <p className="text-xs text-orca-muted mt-1">
              by {plugin.author} &middot; latest v{plugin.latest_version}
              {plugin.license ? ` · ${plugin.license}` : ''}
            </p>
          </div>
          <div className="flex gap-2 shrink-0">
            {installed ? (
              <>
                {canUpdate ? (
                  <Button onClick={openUpdate} disabled={isBusy}>
                    Update
                  </Button>
                ) : null}
                <Button variant="danger" onClick={openUninstall} disabled={isBusy}>
                  Uninstall
                </Button>
              </>
            ) : (
              <Button onClick={openInstall} disabled={isBusy}>
                Install v{plugin.latest_version}
              </Button>
            )}
          </div>
        </div>

        <p className="text-sm mt-4 leading-relaxed whitespace-pre-line">{plugin.description}</p>

        <div className="flex flex-wrap gap-2 mt-4">
          {plugin.homepage ? (
            <Button variant="secondary" size="sm" onClick={() => openExternal(plugin.homepage)}>
              Homepage
            </Button>
          ) : null}
          {plugin.repo ? (
            <Button variant="secondary" size="sm" onClick={() => openExternal(plugin.repo)}>
              Repository
            </Button>
          ) : null}
        </div>
      </Card>

      <Card className="p-5">
        <h3 className="text-sm font-semibold mb-3">Screenshots</h3>
        <ScreenshotCarousel urls={plugin.screenshots} />
      </Card>

      <Card className="p-5">
        <h3 className="text-sm font-semibold mb-3">Permissions requested by v{plugin.latest_version}</h3>
        <PermissionList permissions={latestVersion.permissions} />
      </Card>

      <Card className="p-5">
        <h3 className="text-sm font-semibold mb-3">Version history</h3>
        <VersionTable versions={sortedVersions} installedVersion={installed?.version} />
      </Card>

      <Modal
        open={pendingAction !== null}
        title={
          pendingAction?.kind === 'install'
            ? `Install ${plugin.name} v${pendingAction.version}`
            : pendingAction?.kind === 'update'
              ? `Update ${plugin.name} to v${pendingAction.version}`
              : `Uninstall ${plugin.name}`
        }
        onClose={() => {
          if (!isBusy) setPendingAction(null);
        }}
        footer={
          <>
            <Button
              variant="secondary"
              onClick={() => setPendingAction(null)}
              disabled={isBusy}
            >
              Cancel
            </Button>
            <Button
              variant={pendingAction?.kind === 'uninstall' ? 'danger' : 'primary'}
              onClick={confirmAction}
              disabled={isBusy}
            >
              {isBusy ? 'Working...' : 'Confirm'}
            </Button>
          </>
        }
      >
        {pendingAction?.kind === 'uninstall' ? (
          <p className="text-sm">
            Remove <strong>{plugin.name}</strong> from this OrcaSlicer install? Any plugin data
            stored in its sandbox directory will be deleted.
          </p>
        ) : (
          <div className="space-y-3">
            <p className="text-sm">
              This plugin will be downloaded from the registry, its SHA-256 will be verified, and
              the following permissions will be granted to it:
            </p>
            <PermissionList permissions={latestVersion.permissions} />
            <p className="text-xs text-orca-muted">
              Engine compatibility: <code>{latestVersion.engine_compat}</code>
            </p>
          </div>
        )}
        {resultMessage ? (
          <p className="mt-3 text-sm text-orca-danger">{resultMessage}</p>
        ) : null}
      </Modal>
    </div>
  );
}

export function PluginDetailPage({ id }: PluginDetailPageProps) {
  const catalogQuery = useCatalog();

  if (catalogQuery.isLoading) {
    return <p className="text-orca-muted text-sm">Loading plugin...</p>;
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

  const plugin = catalogQuery.data?.plugins.find((p) => p.id === id);
  if (!plugin) {
    return (
      <Card className="p-6 text-sm text-orca-muted">
        Plugin <code>{id}</code> not found in the current catalog.{' '}
        <a href={hrefFor({ kind: 'catalog' })} className="underline">
          Back to catalog
        </a>
        .
      </Card>
    );
  }

  return <PluginDetailBody plugin={plugin} />;
}
