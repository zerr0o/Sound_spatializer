import { fireEvent, render, screen } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { RangeControl, Toggle } from './Controls';

describe('contrôles UI', () => {
  it('rend un toggle accessible et notifie le changement', () => {
    const onChange = vi.fn();
    render(<Toggle checked={false} onChange={onChange} label="Activer la pièce" />);
    fireEvent.click(screen.getByRole('checkbox', { name: 'Activer la pièce' }));
    expect(onChange).toHaveBeenCalledWith(true);
  });

  it('transmet une valeur numérique depuis le slider', () => {
    const onChange = vi.fn();
    render(<RangeControl label="Azimut" value={30} min={-75} max={75} step={1} unit="°" onChange={onChange} />);
    fireEvent.change(screen.getByRole('slider', { name: 'Azimut' }), { target: { value: '42' } });
    expect(onChange).toHaveBeenCalledWith(42);
  });
});
