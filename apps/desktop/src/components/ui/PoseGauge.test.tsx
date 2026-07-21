import { cleanup, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it } from 'vitest';
import { PoseGauge } from './PoseGauge';

describe('PoseGauge', () => {
  afterEach(cleanup);

  it('trace une valeur négative à gauche du zéro', () => {
    const { container } = render(<PoseGauge axis="yaw" value={-27.7} />);
    const meter = screen.getByRole('meter', { name: 'Yaw : -27.7 degrés' });

    expect(meter).toHaveClass('is-negative');
    expect(meter).toHaveAttribute('data-direction', 'negative');
    expect(meter).toHaveAttribute('aria-valuenow', '-27.7');
    expect(container.querySelector('.pose-ring-progress')).toHaveAttribute(
      'stroke-dasharray',
      `${(27.7 / 90) * 50} ${100 - (27.7 / 90) * 50}`,
    );
  });

  it('trace une valeur positive à droite avec la même amplitude', () => {
    const negative = render(<PoseGauge axis="pitch" value={-23.2} />);
    const negativeArc = negative.container.querySelector('.pose-ring-progress')?.getAttribute('stroke-dasharray');
    negative.unmount();

    const positive = render(<PoseGauge axis="pitch" value={23.2} />);
    const meter = screen.getByRole('meter', { name: 'Pitch : 23.2 degrés' });

    expect(meter).toHaveClass('is-positive');
    expect(meter).toHaveAttribute('data-direction', 'positive');
    expect(positive.container.querySelector('.pose-ring-progress')).toHaveAttribute('stroke-dasharray', negativeArc);
  });

  it('reste neutre à zéro et borne visuellement les poses hors plage', () => {
    const neutral = render(<PoseGauge axis="roll" value={0} />);
    expect(screen.getByRole('meter', { name: 'Roll : 0.0 degrés' })).toHaveClass('is-neutral');
    expect(neutral.container.querySelector('.pose-ring-progress')).toHaveAttribute('stroke-dasharray', '0 100');
    neutral.unmount();

    render(<PoseGauge axis="roll" value={120} />);
    expect(screen.getByRole('meter', { name: 'Roll : 120.0 degrés' })).toHaveAttribute('aria-valuenow', '90');
  });
});

