import { useEffect, useState } from 'react';
import { desktopBridge } from '../lib/tauri-bridge';

/** Vérifie uniquement les ressources locales du paquet ; aucune requête distante. */
export const useHrtfAvailability = () => {
  const [availability, setAvailability] = useState<Record<string, boolean> | null>(null);

  useEffect(() => {
    let active = true;
    const inspect = async () => {
      try {
        const result = await desktopBridge.getBuiltinHrtfAvailability();
        if (active) setAvailability(result);
      } catch {
        if (active) setAvailability({});
      }
    };
    void inspect();
    return () => { active = false; };
  }, []);

  return availability;
};
