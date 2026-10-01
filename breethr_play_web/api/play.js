// Breethr PLAY counter.
//
// GET  /api/play -> {"seq": N}   The panel asks this every few seconds.
// POST /api/play -> {"seq": N+1} The PLAY button. The panel replays when N changes.
//
// N is kept in Upstash Redis (added from the Vercel Marketplace, which sets
// KV_REST_API_URL / KV_REST_API_TOKEN or UPSTASH_REDIS_REST_URL / _TOKEN).
//
// To stay inside the free plans, Vercel's CDN caches the GET answer, so the
// panel's checks almost never reach Redis. A PLAY press deletes that cached
// answer right away. If the delete ever fails, the cache still expires after
// CDN_MAX_AGE_SECONDS, so the panel is never stuck.

import { dangerouslyDeleteByTag } from '@vercel/functions';

const SEQ_KEY = 'breethr:play:seq';
const CACHE_TAG = 'breethr-play';
const CDN_MAX_AGE_SECONDS = 60;

function redisConfig() {
  const url = process.env.KV_REST_API_URL || process.env.UPSTASH_REDIS_REST_URL;
  const token = process.env.KV_REST_API_TOKEN || process.env.UPSTASH_REDIS_REST_TOKEN;
  if (!url || !token) {
    throw new Error('Redis is not connected. Add Upstash Redis to this project in the Vercel dashboard.');
  }
  return { url, token };
}

async function redis(...command) {
  const { url, token } = redisConfig();
  const res = await fetch(url, {
    method: 'POST',
    headers: { Authorization: `Bearer ${token}` },
    body: JSON.stringify(command),
    cache: 'no-store',
  });
  const data = await res.json().catch(() => ({}));
  if (!res.ok || data.error) {
    throw new Error(`Redis error: ${data.error || res.status}`);
  }
  return data.result;
}

function errorResponse(err) {
  console.error(err);
  return Response.json(
    { error: err.message },
    { status: 500, headers: { 'Cache-Control': 'no-store' } },
  );
}

export async function GET() {
  try {
    const seq = Number((await redis('GET', SEQ_KEY)) ?? 0);
    return Response.json(
      { seq },
      {
        headers: {
          // Cached by Vercel's CDN only; browsers and the panel always ask again.
          'Cache-Control': 'no-store',
          'Vercel-CDN-Cache-Control': `max-age=${CDN_MAX_AGE_SECONDS}`,
          'Vercel-Cache-Tag': CACHE_TAG,
        },
      },
    );
  } catch (err) {
    return errorResponse(err);
  }
}

export async function POST() {
  try {
    const seq = Number(await redis('INCR', SEQ_KEY));
    try {
      await dangerouslyDeleteByTag(CACHE_TAG);
    } catch (err) {
      // The panel still picks the press up when the cache expires.
      console.error('Cache purge failed:', err);
    }
    return Response.json({ seq }, { headers: { 'Cache-Control': 'no-store' } });
  } catch (err) {
    return errorResponse(err);
  }
}
