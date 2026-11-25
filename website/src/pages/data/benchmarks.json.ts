import type { APIRoute } from 'astro';
import { dataset } from '../../data/benchmarks';

export const GET: APIRoute = () => {
  const publishable = {
    ...dataset,
    records: dataset.records.filter((r) => r.publishable),
  };
  return new Response(JSON.stringify(publishable, null, 2), {
    headers: { 'Content-Type': 'application/json; charset=utf-8' },
  });
};
