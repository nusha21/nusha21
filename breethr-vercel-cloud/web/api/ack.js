import {
  acknowledgementPath,
  noStore,
  normalizeDeviceId,
  writeJson,
} from "../lib/control-store.js";

export default async function handler(request, response) {
  noStore(response);

  if (request.method !== "POST") {
    response.setHeader("Allow", "POST");
    return response.status(405).json({ error: "Method not allowed" });
  }

  const commandId = String(request.body?.commandId || "").slice(0, 80);
  if (!commandId) {
    return response.status(400).json({ error: "Missing commandId" });
  }

  try {
    const deviceId = normalizeDeviceId(request.body?.device);
    await writeJson(acknowledgementPath(deviceId), {
      commandId,
      acknowledgedAt: new Date().toISOString(),
    });
    return response.status(200).json({ ok: true });
  } catch (error) {
    console.error(error);
    return response.status(500).json({ error: "Could not save acknowledgement" });
  }
}
