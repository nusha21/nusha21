import {
  acknowledgementPath,
  noStore,
  normalizeDeviceId,
  readJson,
} from "../lib/control-store.js";

export default async function handler(request, response) {
  noStore(response);

  if (request.method !== "GET") {
    response.setHeader("Allow", "GET");
    return response.status(405).json({ error: "Method not allowed" });
  }

  try {
    const deviceId = normalizeDeviceId(request.query?.device);
    const expectedCommandId = String(request.query?.commandId || "");
    const acknowledgement = await readJson(acknowledgementPath(deviceId));
    const acknowledged = Boolean(
      expectedCommandId && acknowledgement?.commandId === expectedCommandId,
    );

    return response.status(200).json({
      acknowledged,
      acknowledgedAt: acknowledged
        ? acknowledgement.acknowledgedAt
        : null,
    });
  } catch (error) {
    console.error(error);
    return response.status(500).json({ error: "Could not read status" });
  }
}
