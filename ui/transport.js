// Device commands are serialized. Trial persistence never occupies this queue.
export const REQUEST_TIMEOUT_MS = 3000;
export class DeviceClient {
  constructor(fetcher = (...args) => fetch(...args)) {
    this.fetcher = fetcher;
    this.queue = Promise.resolve();
    this.poll = null;
  }
  async send(data, endpoint, timeoutMs = REQUEST_TIMEOUT_MS) {
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), timeoutMs);
    try {
      const response = await this.fetcher(endpoint, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(data),
        signal: controller.signal,
      });
      const result = await response.json();
      if (!response.ok) throw new Error(result.error || 'Request failed');
      if (endpoint === '/api/device' && typeof result.ok !== 'boolean') {
        throw new Error('Malformed device acknowledgement');
      }
      return result;
    } catch (error) {
      if (error.name === 'AbortError') throw new Error('Request timed out after three seconds');
      throw error;
    } finally {
      clearTimeout(timeout);
    }
  }
  request(data, endpoint = '/api/device') {
    if (endpoint !== '/api/device') return this.send(data, endpoint);
    const polling = ['status', 'step'].includes(data.action);
    if (polling && this.poll) return this.poll;
    const deadline = performance.now() + REQUEST_TIMEOUT_MS;
    const task = this.queue.then(() => {
      const remainingMs = deadline - performance.now();
      if (remainingMs <= 0) throw new Error('Queued request expired after three seconds');
      return this.send(data, endpoint, remainingMs);
    });
    this.queue = task.catch(() => {});
    if (polling) {
      this.poll = task;
      task
        .finally(() => {
          this.poll = null;
        })
        .catch(() => {});
    }
    return task;
  }
}
