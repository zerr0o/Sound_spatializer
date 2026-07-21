import type { ReactNode } from 'react';
import { Check, ChevronDown } from 'lucide-react';

export function Toggle({
  checked,
  onChange,
  label,
  description,
  disabled = false,
}: {
  checked: boolean;
  onChange: (checked: boolean) => void;
  label: string;
  description?: string;
  disabled?: boolean;
}) {
  return (
    <label className={`toggle-row ${disabled ? 'is-disabled' : ''}`}>
      <span className="toggle-copy">
        <strong>{label}</strong>
        {description && <small>{description}</small>}
      </span>
      <input
        className="sr-only"
        type="checkbox"
        checked={checked}
        onChange={(event) => onChange(event.target.checked)}
        disabled={disabled}
      />
      <span className={`toggle ${checked ? 'is-on' : ''}`} aria-hidden="true">
        <span />
      </span>
    </label>
  );
}

export function RangeControl({
  label,
  value,
  min,
  max,
  step,
  unit,
  onChange,
  hint,
}: {
  label: string;
  value: number;
  min: number;
  max: number;
  step: number;
  unit: string;
  onChange: (value: number) => void;
  hint?: string;
}) {
  const fill = ((value - min) / (max - min)) * 100;
  return (
    <label className="range-control">
      <span className="range-heading">
        <span>{label}</span>
        <strong>
          {Number.isInteger(step) ? value.toFixed(0) : value.toFixed(1)} <small>{unit}</small>
        </strong>
      </span>
      <input
        type="range"
        aria-label={label}
        min={min}
        max={max}
        step={step}
        value={value}
        style={{ '--range-fill': `${fill}%` } as React.CSSProperties}
        onChange={(event) => onChange(Number(event.target.value))}
      />
      {hint && <small className="control-hint">{hint}</small>}
    </label>
  );
}

export function SelectControl({
  label,
  value,
  onChange,
  children,
  disabled = false,
}: {
  label: string;
  value: string;
  onChange: (value: string) => void;
  children: ReactNode;
  disabled?: boolean;
}) {
  return (
    <label className="select-control">
      <span>{label}</span>
      <span className="select-shell">
        <select value={value} onChange={(event) => onChange(event.target.value)} disabled={disabled}>
          {children}
        </select>
        <ChevronDown size={15} aria-hidden="true" />
      </span>
    </label>
  );
}

export function SegmentedControl<T extends string>({
  value,
  options,
  onChange,
  ariaLabel,
  disabled = false,
}: {
  value: T;
  options: ReadonlyArray<{ value: T; label: string; icon?: ReactNode; disabled?: boolean; title?: string }>;
  onChange: (value: T) => void;
  ariaLabel: string;
  disabled?: boolean;
}) {
  return (
    <div className="segmented-control" role="radiogroup" aria-label={ariaLabel} aria-busy={disabled || undefined}>
      {options.map((option) => (
        <button
          key={option.value}
          type="button"
          role="radio"
          aria-checked={option.value === value}
          className={option.value === value ? 'is-selected' : ''}
          onClick={() => onChange(option.value)}
          disabled={disabled || option.disabled}
          title={option.title}
        >
          {option.icon}
          {option.label}
        </button>
      ))}
    </div>
  );
}

export function StepMarker({ status, index }: { status: 'done' | 'current' | 'future'; index: number }) {
  return <span className={`step-marker is-${status}`}>{status === 'done' ? <Check size={14} /> : index + 1}</span>;
}

export function EmptyState({ icon, title, description, action }: { icon: ReactNode; title: string; description: string; action?: ReactNode }) {
  return (
    <div className="empty-state">
      <span className="empty-state-icon">{icon}</span>
      <strong>{title}</strong>
      <p>{description}</p>
      {action}
    </div>
  );
}
