import type { ReactNode } from 'react';

export function MetricCard({
  label,
  value,
  unit,
  hint,
  tone = 'neutral',
  icon,
}: {
  label: string;
  value: string | number;
  unit?: string;
  hint?: string;
  tone?: 'neutral' | 'good' | 'warning' | 'danger';
  icon?: ReactNode;
}) {
  return (
    <article className={`metric-card tone-${tone}`}>
      <div className="metric-label">
        {icon}
        <span>{label}</span>
      </div>
      <div className="metric-value">
        {value}
        {unit && <small>{unit}</small>}
      </div>
      {hint && <p>{hint}</p>}
    </article>
  );
}

export function StatusDot({
  state,
  label,
}: {
  state: 'active' | 'warning' | 'offline' | 'error';
  label: string;
}) {
  return (
    <span className={`status-dot status-${state}`}>
      <i aria-hidden="true" />
      {label}
    </span>
  );
}
