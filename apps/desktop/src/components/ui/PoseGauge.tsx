const MAX_POSE_DEGREES = 90;

type PoseAxis = 'yaw' | 'pitch' | 'roll';

const AXIS_LABELS: Record<PoseAxis, string> = {
  yaw: 'Yaw',
  pitch: 'Pitch',
  roll: 'Roll',
};

export function PoseGauge({ axis, value }: { axis: PoseAxis; value: number }) {
  const safeValue = Number.isFinite(value) ? value : 0;
  const clampedValue = Math.max(-MAX_POSE_DEGREES, Math.min(MAX_POSE_DEGREES, safeValue));
  // Chaque signe possède sa propre moitié du cadran : zéro en haut,
  // négatif vers la gauche et positif vers la droite.
  const arcLength = (Math.abs(clampedValue) / MAX_POSE_DEGREES) * 50;
  const direction = clampedValue < 0 ? 'negative' : clampedValue > 0 ? 'positive' : 'neutral';
  const formattedValue = safeValue.toFixed(1);

  return (
    <div className="pose-gauge">
      <span
        className={`pose-ring is-${direction}`}
        role="meter"
        aria-label={`${AXIS_LABELS[axis]} : ${formattedValue} degrés`}
        aria-valuemin={-MAX_POSE_DEGREES}
        aria-valuemax={MAX_POSE_DEGREES}
        aria-valuenow={clampedValue}
        aria-valuetext={`${formattedValue} degrés`}
        data-direction={direction}
      >
        <svg className="pose-ring-svg" viewBox="0 0 64 64" aria-hidden="true">
          <circle className="pose-ring-track" cx="32" cy="32" r="28" pathLength="100" />
          <circle
            className="pose-ring-progress"
            cx="32"
            cy="32"
            r="28"
            pathLength="100"
            strokeDasharray={`${arcLength} ${100 - arcLength}`}
          />
        </svg>
        <strong>{formattedValue}°</strong>
      </span>
      <small>{axis.toUpperCase()}</small>
    </div>
  );
}

