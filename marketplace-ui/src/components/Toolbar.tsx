import type { PluginCategory, PluginKind } from '../types';

export type SortMode = 'name' | 'recent';

export interface ToolbarFilters {
  search: string;
  category: PluginCategory | 'all';
  kind: PluginKind | 'all';
  sort: SortMode;
}

interface ToolbarProps {
  value: ToolbarFilters;
  onChange: (next: ToolbarFilters) => void;
  resultCount: number;
}

const CATEGORY_OPTIONS: Array<{ value: PluginCategory | 'all'; label: string }> = [
  { value: 'all', label: 'All categories' },
  { value: 'device', label: 'Device agent' },
  { value: 'profile_pack', label: 'Profile pack' },
  { value: 'gcode', label: 'G-code filter' },
  { value: 'ui', label: 'UI extension' },
  { value: 'misc', label: 'Miscellaneous' },
];

const KIND_OPTIONS: Array<{ value: PluginKind | 'all'; label: string }> = [
  { value: 'all', label: 'All kinds' },
  { value: 'native', label: 'Native' },
  { value: 'wasm', label: 'WASM' },
  { value: 'webview', label: 'Webview' },
  { value: 'hybrid', label: 'Hybrid' },
  { value: 'data', label: 'Data' },
];

const SORT_OPTIONS: Array<{ value: SortMode; label: string }> = [
  { value: 'name', label: 'Sort: Name' },
  { value: 'recent', label: 'Sort: Most recent' },
];

const SELECT_CLASSES =
  'bg-orca-panel border border-orca-border rounded-md px-2.5 py-1.5 text-sm text-orca-text focus:outline-none focus-visible:ring-2 focus-visible:ring-orca-accent';

export function Toolbar({ value, onChange, resultCount }: ToolbarProps) {
  return (
    <div className="flex flex-wrap items-center gap-2 p-3 bg-orca-panel border border-orca-border rounded-md">
      <input
        type="search"
        placeholder="Search by name, description, or tag"
        value={value.search}
        onChange={(e) => onChange({ ...value, search: e.target.value })}
        className="flex-1 min-w-[220px] bg-orca-bg border border-orca-border rounded-md px-3 py-1.5 text-sm focus:outline-none focus-visible:ring-2 focus-visible:ring-orca-accent"
      />

      <select
        value={value.category}
        onChange={(e) => onChange({ ...value, category: e.target.value as ToolbarFilters['category'] })}
        className={SELECT_CLASSES}
        aria-label="Filter by category"
      >
        {CATEGORY_OPTIONS.map((opt) => (
          <option key={opt.value} value={opt.value}>
            {opt.label}
          </option>
        ))}
      </select>

      <select
        value={value.kind}
        onChange={(e) => onChange({ ...value, kind: e.target.value as ToolbarFilters['kind'] })}
        className={SELECT_CLASSES}
        aria-label="Filter by plugin kind"
      >
        {KIND_OPTIONS.map((opt) => (
          <option key={opt.value} value={opt.value}>
            {opt.label}
          </option>
        ))}
      </select>

      <select
        value={value.sort}
        onChange={(e) => onChange({ ...value, sort: e.target.value as SortMode })}
        className={SELECT_CLASSES}
        aria-label="Sort"
      >
        {SORT_OPTIONS.map((opt) => (
          <option key={opt.value} value={opt.value}>
            {opt.label}
          </option>
        ))}
      </select>

      <span className="text-xs text-orca-muted ml-auto">
        {resultCount} {resultCount === 1 ? 'plugin' : 'plugins'}
      </span>
    </div>
  );
}
