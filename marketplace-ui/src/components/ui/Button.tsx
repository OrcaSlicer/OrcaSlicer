import type { ButtonHTMLAttributes, ReactNode } from 'react';

type ButtonVariant = 'primary' | 'secondary' | 'danger' | 'ghost';
type ButtonSize = 'sm' | 'md';

interface ButtonProps extends ButtonHTMLAttributes<HTMLButtonElement> {
  variant?: ButtonVariant;
  size?: ButtonSize;
  children: ReactNode;
}

const VARIANT_CLASSES: Record<ButtonVariant, string> = {
  primary:
    'bg-orca-accent text-white hover:bg-orca-accent-hover disabled:bg-orca-border disabled:text-orca-muted',
  secondary:
    'bg-orca-panel border border-orca-border text-orca-text hover:border-orca-muted disabled:opacity-60',
  danger:
    'bg-orca-danger text-white hover:bg-red-400 disabled:bg-orca-border disabled:text-orca-muted',
  ghost:
    'bg-transparent text-orca-text hover:bg-orca-panel disabled:text-orca-muted',
};

const SIZE_CLASSES: Record<ButtonSize, string> = {
  sm: 'px-2.5 py-1 text-xs rounded',
  md: 'px-3.5 py-1.5 text-sm rounded-md',
};

export function Button({
  variant = 'primary',
  size = 'md',
  className = '',
  children,
  ...rest
}: ButtonProps) {
  const classes = [
    'inline-flex items-center justify-center font-medium transition-colors',
    'focus:outline-none focus-visible:ring-2 focus-visible:ring-orca-accent focus-visible:ring-offset-1 focus-visible:ring-offset-orca-bg',
    'disabled:cursor-not-allowed',
    VARIANT_CLASSES[variant],
    SIZE_CLASSES[size],
    className,
  ]
    .filter(Boolean)
    .join(' ');

  return (
    <button {...rest} className={classes}>
      {children}
    </button>
  );
}
