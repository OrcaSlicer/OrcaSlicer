import { useEffect, type ReactNode } from 'react';

interface ModalProps {
  open: boolean;
  title: string;
  onClose: () => void;
  children: ReactNode;
  footer?: ReactNode;
}

export function Modal({ open, title, onClose, children, footer }: ModalProps) {
  useEffect(() => {
    if (!open) return;
    function onKey(e: KeyboardEvent) {
      if (e.key === 'Escape') onClose();
    }
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [open, onClose]);

  if (!open) return null;

  return (
    <div
      className="fixed inset-0 z-50 flex items-center justify-center bg-black/60 p-4"
      onClick={onClose}
      role="presentation"
    >
      <div
        className="bg-orca-panel border border-orca-border rounded-lg shadow-2xl w-full max-w-lg max-h-[90vh] flex flex-col"
        onClick={(e) => e.stopPropagation()}
        role="dialog"
        aria-modal="true"
        aria-labelledby="orca-modal-title"
      >
        <div className="px-5 py-3 border-b border-orca-border flex items-center justify-between">
          <h2 id="orca-modal-title" className="text-base font-semibold">
            {title}
          </h2>
          <button
            type="button"
            onClick={onClose}
            className="text-orca-muted hover:text-orca-text text-lg leading-none"
            aria-label="Close"
          >
            &times;
          </button>
        </div>
        <div className="px-5 py-4 overflow-auto flex-1">{children}</div>
        {footer ? (
          <div className="px-5 py-3 border-t border-orca-border flex gap-2 justify-end">
            {footer}
          </div>
        ) : null}
      </div>
    </div>
  );
}
