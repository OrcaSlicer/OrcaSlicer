import type { HTMLAttributes, ReactNode } from 'react';

type ChipTone = 'neutral' | 'accent' | 'success' | 'warning' | 'danger';

interface ChipProps extends HTMLAttributes<HTMLSpanElement> {
  tone?: ChipTone;
  children: ReactNode;
}

const TONE_CLASSES: Record<ChipTone, string> = {
  neutral: 'bg-orca-border text-orca-text',
  accent: 'bg-orca-accent/15 text-orca-accent border border-orca-accent/40',
  success: 'bg-orca-success/15 text-orca-success border border-orca-success/40',
  warning: 'bg-orca-warning/15 text-orca-warning border border-orca-warning/40',
  danger: 'bg-orca-danger/15 text-orca-danger border border-orca-danger/40',
};

export function Chip({ tone = 'neutral', className = '', children, ...rest }: ChipProps) {
  const classes = [
    'inline-flex items-center px-2 py-0.5 text-[11px] font-medium rounded-full whitespace-nowrap',
    TONE_CLASSES[tone],
    className,
  ]
    .filter(Boolean)
    .join(' ');
  return (
    <span {...rest} className={classes}>
      {children}
    </span>
  );
}
