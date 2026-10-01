import {
  commandPath,
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
    const command = await readJson(commandPath(deviceId));
    return response.status(200).json(
      command || { commandId: "", action: "none", issuedAt: null },
    );
  } catch (error) {
    console.error(error);
    return response.status(500).json({ error: "Could not read command" });
  }
}
