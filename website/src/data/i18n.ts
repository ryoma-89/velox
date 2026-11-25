import type { Locale } from './schema';

export const SITE_URL = 'https://ryoma-89.github.io';
export const BASE_PATH = '/velox';
export const GITHUB_REPO_URL = 'https://github.com/ryoma-89/velox';

export type RouteSlug =
  | ''
  | 'features'
  | 'improvements'
  | 'benchmarks'
  | 'architecture'
  | 'history'
  | 'docs';

export interface NavItem {
  slug: RouteSlug;
  label: { ja: string; en: string };
}

export const NAV_ITEMS: NavItem[] = [
  { slug: '', label: { ja: 'ホーム', en: 'Home' } },
  { slug: 'improvements', label: { ja: '公式からの改善', en: 'Improvements' } },
  { slug: 'benchmarks', label: { ja: 'ベンチマーク', en: 'Benchmarks' } },
  { slug: 'architecture', label: { ja: '技術・構造', en: 'Architecture' } },
  { slug: 'features', label: { ja: '特徴・デスクトップ', en: 'Features' } },
  { slug: 'history', label: { ja: '開発の歴史', en: 'History' } },
  { slug: 'docs', label: { ja: '導入・ドキュメント', en: 'Docs' } },
];

export function localizedPath(slug: RouteSlug, locale: Locale): string {
  const prefix = locale === 'ja' ? `${BASE_PATH}/` : `${BASE_PATH}/en/`;
  if (!slug) return prefix;
  return `${prefix}${slug}/`;
}

export function fullUrl(slug: RouteSlug, locale: Locale): string {
  return `${SITE_URL}${localizedPath(slug, locale)}`;
}

export function assetPath(rel: string): string {
  const clean = rel.replace(/^\/+/, '');
  return `${BASE_PATH}/${clean}`;
}
