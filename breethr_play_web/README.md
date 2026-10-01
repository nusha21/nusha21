# Breethr PLAY – web app

A single page with the Breethr logo and one **PLAY** button. Each press makes the panel
(`breethr_play_once` firmware) play its animation once.

```
public/index.html   the page
public/logo.svg     Breethr logomark
api/play.js         GET  -> {"seq": N}  (the panel checks this every 3 s)
                    POST -> adds 1 to N (the PLAY button)
```

## Deploy to Vercel (one time)

1. Push this repo to GitHub (already done if you're reading this there).
2. On vercel.com: **Add New → Project** → import the repo.
   Set **Root Directory** to `breethr_play_web`. Leave the other settings as they are. Click **Deploy**.
3. In the new project: **Storage → Create Database → Upstash for Redis** (free plan) →
   connect it to this project. This adds the `KV_REST_API_URL` / `KV_REST_API_TOKEN`
   variables automatically.
4. **Deployments → ⋯ → Redeploy** so the new variables are picked up.
5. Copy the project address (e.g. `https://breethr-play.vercel.app`) into
   `PLAY_SERVER_URL` in `breethr_play_once.ino` and upload the firmware.

Check it works: open `https://<your-address>/api/play` → you should see `{"seq":0}`.
If you see an error about Redis, step 3 or 4 was missed.

## Free plan limits

The panel asks every 3 s, about 870,000 times a month. Vercel's free plan allows
1,000,000 requests a month. Vercel's CDN caches the answer, so these checks don't use
Redis. Pressing PLAY clears the cached answer, so the panel sees the press on its next check.
If clearing the cache ever fails, the cache still expires within 60 s.

## Notes

- Anyone with the link can press PLAY.
- The page uses Hanken Grotesk (Google Fonts) as a free stand-in for Lay Grotesk.
  It uses the guide colours Warm White, Breethr Sky, Blues and the logo blue `#073D4D`.
