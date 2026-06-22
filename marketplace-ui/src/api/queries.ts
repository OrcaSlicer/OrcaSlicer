import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { bridge } from './bridge';
import type { ActionResult, InstalledResponse, MarketplaceCatalog } from '../types';

const FIVE_MINUTES = 5 * 60 * 1000;
const THIRTY_SECONDS = 30 * 1000;

export const queryKeys = {
  catalog: ['catalog'] as const,
  installed: ['installed'] as const,
};

export function useCatalog() {
  return useQuery<MarketplaceCatalog>({
    queryKey: queryKeys.catalog,
    queryFn: () => bridge.list(),
    staleTime: FIVE_MINUTES,
    gcTime: FIVE_MINUTES * 2,
  });
}

export function useInstalled() {
  return useQuery<InstalledResponse>({
    queryKey: queryKeys.installed,
    queryFn: () => bridge.installed(),
    staleTime: THIRTY_SECONDS,
    gcTime: THIRTY_SECONDS * 4,
  });
}

interface VersionedArgs {
  id: string;
  version: string;
}

export function useInstall() {
  const qc = useQueryClient();
  return useMutation<ActionResult, Error, VersionedArgs>({
    mutationFn: ({ id, version }) => bridge.install(id, version),
    onSuccess: (result) => {
      if (result.status === 'ok') {
        void qc.invalidateQueries({ queryKey: queryKeys.installed });
      }
    },
  });
}

export function useUpdatePlugin() {
  const qc = useQueryClient();
  return useMutation<ActionResult, Error, VersionedArgs>({
    mutationFn: ({ id, version }) => bridge.update(id, version),
    onSuccess: (result) => {
      if (result.status === 'ok') {
        void qc.invalidateQueries({ queryKey: queryKeys.installed });
      }
    },
  });
}

export function useUninstall() {
  const qc = useQueryClient();
  return useMutation<ActionResult, Error, string>({
    mutationFn: (id) => bridge.uninstall(id),
    onSuccess: (result) => {
      if (result.status === 'ok') {
        void qc.invalidateQueries({ queryKey: queryKeys.installed });
      }
    },
  });
}
