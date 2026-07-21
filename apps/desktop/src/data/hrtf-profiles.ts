export interface HrtfProfile {
  id: string;
  name: string;
  subject: string;
  description: string;
  traits: string[];
  accent: string;
  builtIn: boolean;
}

export const HRTF_PROFILES: HrtfProfile[] = [
  {
    id: 'sadie-d2-kemar',
    name: 'KEMAR — référence',
    subject: 'SADIE II / D2',
    description: 'Mesure sur tête artificielle incluse comme référence commune pour la comparaison.',
    traits: ['Tête artificielle', 'Référence'],
    accent: '#64e6cf',
    builtIn: true,
  },
  {
    id: 'sadie-h6',
    name: 'Humain A',
    subject: 'SADIE II / H6',
    description: 'Mesure individuelle anonymisée à évaluer avec l’assistant d’écoute A/B.',
    traits: ['Sujet humain', 'Profil A'],
    accent: '#78a9ff',
    builtIn: true,
  },
  {
    id: 'sadie-h9',
    name: 'Humain B',
    subject: 'SADIE II / H9',
    description: 'Mesure individuelle anonymisée à évaluer avec l’assistant d’écoute A/B.',
    traits: ['Sujet humain', 'Profil B'],
    accent: '#d9a7ff',
    builtIn: true,
  },
  {
    id: 'sadie-h10',
    name: 'Humain C',
    subject: 'SADIE II / H10',
    description: 'Mesure individuelle anonymisée à évaluer avec l’assistant d’écoute A/B.',
    traits: ['Sujet humain', 'Profil C'],
    accent: '#ffc46b',
    builtIn: true,
  },
  {
    id: 'sadie-h19',
    name: 'Humain D',
    subject: 'SADIE II / H19',
    description: 'Mesure individuelle anonymisée à évaluer avec l’assistant d’écoute A/B.',
    traits: ['Sujet humain', 'Profil D'],
    accent: '#ff8a84',
    builtIn: true,
  },
  {
    id: 'sadie-h20',
    name: 'Humain E',
    subject: 'SADIE II / H20',
    description: 'Mesure individuelle anonymisée à évaluer avec l’assistant d’écoute A/B.',
    traits: ['Sujet humain', 'Profil E'],
    accent: '#a7df78',
    builtIn: true,
  },
];
