// chhaya-yt-search: YouTube search that only returns videos which can actually play inside
// the Chhaya page (embeddable + playable outside youtube.com). Uses the YouTube Data API key
// that is already stored in Supabase secrets; the key never reaches the app.
//
// Request:  POST { "query": "lag ja gale", "max": 6 }
// Response: { "items": [ { "videoId": "...", "title": "...", "channel": "..." }, ... ] }

// Uses whichever secret holds the YouTube key: a known name first, otherwise any secret whose
// name mentions YouTube/YT/Google and whose value looks like a Google API key ("AIza...").
function findKey(): string {
  for (const n of ["YOUTUBE_API_KEY", "YT_API_KEY", "YOUTUBE_KEY", "GOOGLE_API_KEY"]) {
    const v = Deno.env.get(n); if (v) return v;
  }
  const env = Deno.env.toObject();
  const named = Object.entries(env).find(([n, v]) => /youtube|(^|_)yt(_|$)|google/i.test(n) && v.startsWith("AIza"));
  if (named) return named[1];
  const anyGoogle = Object.values(env).find((v) => /^AIza[0-9A-Za-z_-]{30,}$/.test(v));
  return anyGoogle ?? "";
}
const KEY = findKey();

const cors = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Headers": "authorization, apikey, content-type, x-client-info",
  "Access-Control-Allow-Methods": "POST, OPTIONS",
};
const json = (body: unknown, status = 200) =>
  new Response(JSON.stringify(body), { status, headers: { ...cors, "Content-Type": "application/json" } });

// YouTube returns titles with HTML entities (&amp; &#39; ...)
const decode = (s: string) =>
  s.replace(/&amp;/g, "&").replace(/&quot;/g, '"').replace(/&#39;/g, "'").replace(/&lt;/g, "<").replace(/&gt;/g, ">");

Deno.serve(async (req) => {
  if (req.method === "OPTIONS") return new Response("ok", { headers: cors });
  try {
    const { query, max = 6 } = await req.json();
    if (!query || typeof query !== "string") return json({ error: "query is required" }, 400);
    if (!KEY) return json({ error: "No YouTube key found in Supabase secrets (add one named YOUTUBE_API_KEY)" }, 500);

    const u = new URL("https://www.googleapis.com/youtube/v3/search");
    u.search = new URLSearchParams({
      part: "snippet",
      type: "video",
      videoEmbeddable: "true",   // can be embedded in a page
      videoSyndicated: "true",   // can be played outside youtube.com
      maxResults: String(Math.min(Math.max(Number(max) || 6, 1), 10)),
      q: query.slice(0, 200),
      regionCode: "IN",
      relevanceLanguage: "hi",
      safeSearch: "moderate",
      key: KEY,
    }).toString();

    const r = await fetch(u);
    const d = await r.json();
    if (!r.ok) return json({ error: d?.error?.message || `YouTube error ${r.status}` }, 502);

    const items = (d.items || [])
      .filter((i: any) => i?.id?.videoId && i?.snippet?.liveBroadcastContent !== "live")
      .map((i: any) => ({ videoId: i.id.videoId, title: decode(i.snippet.title || ""), channel: i.snippet.channelTitle || "" }));
    return json({ items });
  } catch (e) {
    return json({ error: String((e as Error)?.message || e) }, 500);
  }
});
