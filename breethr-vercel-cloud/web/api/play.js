import { randomUUID } from "node:crypto";
import {
  commandPath,
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

  try {
    const deviceId = normalizeDeviceId(request.body?.device);
    const command = {
      commandId: `${Date.now()}-${randomUUID().slice(0, 8)}`,
      action: "play",
      issuedAt: new Date().toISOString(),
    };

    await writeJson(commandPath(deviceId), command);
    return response.status(202).json({
      ok: true,
      device: deviceId,
      commandId: command.commandId,
    });
  } catch (error) {
    console.error(error);
    return response.status(500).json({
      error: "Cloud storage is unavailable. Check the Blob integration.",
    });
  }
}
