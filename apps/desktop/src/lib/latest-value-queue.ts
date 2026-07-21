export interface LatestValueQueueOptions {
  /** Nombre maximal d'envois qui attendent simultanément leur accusé Tauri. */
  maxInFlight?: number;
  /** Reçoit chaque résultat sans laisser de Promise rejetée non observée. */
  onSettled?: (error: unknown | null) => void;
}

/**
 * File latest-only bornée.
 *
 * Les appels IPC peuvent être pipelinés afin que leur aller-retour ne limite pas
 * la cadence caméra. Une fois les slots occupés, une unique valeur en attente est
 * conservée et remplacée par la plus récente. La mémoire utilisée reste donc
 * bornée à `maxInFlight + 1` valeurs, même si un consommateur se bloque.
 */
export class LatestValueQueue<T> {
  private pending: T | undefined;
  private inFlight = 0;
  private readonly maxInFlight: number;
  private readonly onSettled: (error: unknown | null) => void;

  constructor(
    private readonly consume: (value: T) => Promise<void>,
    options: LatestValueQueueOptions = {},
  ) {
    this.maxInFlight = Math.max(1, Math.floor(options.maxInFlight ?? 1));
    this.onSettled = options.onSettled ?? (() => undefined);
  }

  push(value: T): void {
    if (this.inFlight < this.maxInFlight) {
      this.start(value);
      return;
    }
    this.pending = value;
  }

  private start(value: T): void {
    this.inFlight += 1;
    void Promise.resolve()
      .then(() => this.consume(value))
      .then(
        () => this.notifySettled(null),
        (error: unknown) => this.notifySettled(error),
      )
      .finally(() => {
        this.inFlight -= 1;
        if (this.pending === undefined) return;
        const latest = this.pending;
        this.pending = undefined;
        this.start(latest);
      });
  }

  private notifySettled(error: unknown | null): void {
    try {
      this.onSettled(error);
    } catch {
      // Un observateur de diagnostic ne doit ni bloquer la file ni produire
      // un rejet de Promise non observé.
    }
  }
}
