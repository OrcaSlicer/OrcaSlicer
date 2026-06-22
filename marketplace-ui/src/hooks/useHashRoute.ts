import { useEffect, useState } from 'react';

export type Route =
  | { kind: 'catalog' }
  | { kind: 'installed' }
  | { kind: 'detail'; id: string }
  | { kind: 'unknown'; hash: string };

function parseHash(raw: string): Route {
  const hash = raw.replace(/^#\/?/, '');
  if (hash === '' || hash === '/') return { kind: 'catalog' };
  if (hash === 'installed') return { kind: 'installed' };
  const detailMatch = hash.match(/^plugin\/([^/?#]+)$/);
  if (detailMatch && detailMatch[1]) {
    return { kind: 'detail', id: decodeURIComponent(detailMatch[1]) };
  }
  return { kind: 'unknown', hash };
}

function currentHash(): string {
  if (typeof window === 'undefined') return '';
  return window.location.hash;
}

export function useHashRoute(): Route {
  const [route, setRoute] = useState<Route>(() => parseHash(currentHash()));

  useEffect(() => {
    function onChange() {
      setRoute(parseHash(currentHash()));
    }
    window.addEventListener('hashchange', onChange);
    return () => window.removeEventListener('hashchange', onChange);
  }, []);

  return route;
}

export function navigate(target: string): void {
  if (typeof window === 'undefined') return;
  const normalized = target.startsWith('#') ? target : `#${target}`;
  window.location.hash = normalized.replace(/^#/, '');
}

export function hrefFor(route: Exclude<Route, { kind: 'unknown' }>): string {
  switch (route.kind) {
    case 'catalog':
      return '#/';
    case 'installed':
      return '#/installed';
    case 'detail':
      return `#/plugin/${encodeURIComponent(route.id)}`;
  }
}
