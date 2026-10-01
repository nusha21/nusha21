import { get, put } from "@vercel/blob";

export const DEFAULT_DEVICE_ID = "breethr-esp32";

export const normalizeDeviceId = (value) => {
  const candidate = String(value || DEFAULT_DEVICE_ID)
    .trim()
    .toLowerCase();
  return /^[a-z0-9-]{1,40}$/.test(candidate)
    ? candidate
    : DEFAULT_DEVICE_ID;
};

export const commandPath = (deviceId) =>
  `breethr-control/${deviceId}/command.json`;

export const acknowledgementPath = (deviceId) =>
  `breethr-control/${deviceId}/acknowledgement.json`;

export const readJson = async (pathname) => {
  const result = await get(pathname, { access: "private" });
  if (!result || result.statusCode !== 200) return null;

  const text = await new Response(result.stream).text();
  return JSON.parse(text);
};

export const writeJson = async (pathname, value) =>
  put(pathname, JSON.stringify(value), {
    access: "private",
    addRandomSuffix: false,
    allowOverwrite: true,
    contentType: "application/json",
    cacheControlMaxAge: 60,
  });

export const noStore = (response) => {
  response.setHeader("Cache-Control", "no-store, max-age=0");
};
