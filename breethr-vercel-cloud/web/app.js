const DEVICE_ID = "breethr-esp32";
const ACK_TIMEOUT_MS = 20000;
const ACK_POLL_MS = 1000;

const playButton = document.querySelector("#play-button");
const buttonLabel = playButton.querySelector(".play-button__label");
const status = document.querySelector("#status");

const setState = (message, state = "") => {
  status.textContent = message;
  status.dataset.state = state;
};

const setStarting = (starting) => {
  playButton.disabled = starting;
  playButton.classList.toggle("is-starting", starting);
  buttonLabel.textContent = starting ? "Starting…" : "Play animation";
};

const delay = (milliseconds) =>
  new Promise((resolve) => window.setTimeout(resolve, milliseconds));

const waitForAcknowledgement = async (commandId) => {
  const deadline = Date.now() + ACK_TIMEOUT_MS;

  while (Date.now() < deadline) {
    await delay(ACK_POLL_MS);

    const query = new URLSearchParams({
      device: DEVICE_ID,
      commandId,
    });
    const response = await fetch(`/api/status?${query}`, {
      cache: "no-store",
    });

    if (!response.ok) continue;
    const result = await response.json();
    if (result.acknowledged) return true;
  }

  return false;
};

const playAnimation = async () => {
  setStarting(true);
  setState("Sending command through Vercel…");

  try {
    const response = await fetch("/api/play", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ device: DEVICE_ID }),
    });

    if (!response.ok) {
      const error = await response.json().catch(() => ({}));
      throw new Error(error.error || `Cloud returned ${response.status}`);
    }

    const { commandId } = await response.json();
    setState("Command sent. Waiting for the panel…");

    const acknowledged = await waitForAcknowledgement(commandId);
    setState(
      acknowledged
        ? "Animation playing"
        : "Command queued — the panel will play when it reconnects",
      acknowledged ? "success" : "queued",
    );
  } catch (error) {
    console.error(error);
    setState("Could not reach the Breethr cloud controller", "error");
  } finally {
    setStarting(false);
  }
};

playButton.addEventListener("click", playAnimation);
