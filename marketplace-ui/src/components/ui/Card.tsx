import type { HTMLAttributes, ReactNode } from 'react';

interface CardProps extends HTMLAttributes<HTMLDivElement> {
  children: ReactNode;
}

export function Card({ children, className = '', ...rest }: CardProps) {
  const classes = [
    'bg-orca-panel border border-orca-border rounded-lg',
    className,
  ]
    .filter(Boolean)
    .join(' ');
  return (
    <div {...rest} className={classes}>
      {children}
    </div>
  );
}
