class PCM16StreamPlayer extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const config = options.processorOptions || {};
    this.inputSampleRate = Number(config.inputSampleRate || 16000);
    this.frameSamples = Number(config.frameSamples || 320);
    this.targetSamples = this.frameSamples * Number(config.targetBufferFrames || 3);
    this.capacity = this.frameSamples * Number(config.maxBufferFrames || 8);
    this.buffer = new Float32Array(this.capacity);
    this.readIndex = 0;
    this.writeIndex = 0;
    this.available = 0;
    this.phase = 0;
    this.started = false;
    this.underruns = 0;
    this.droppedSamples = 0;
    this.lastStatsFrame = 0;
    this.port.onmessage = event => {
      if (event.data?.type === "reset") {
        this.clearBuffer();
      } else if (event.data?.type === "samples") {
        this.enqueue(event.data.samples);
      }
    };
  }

  clearBuffer() {
    this.readIndex = 0;
    this.writeIndex = 0;
    this.available = 0;
    this.phase = 0;
    this.started = false;
  }

  enqueue(samples) {
    if (!(samples instanceof Float32Array)) return;
    for (let index = 0; index < samples.length; index++) {
      if (this.available >= this.capacity) {
        this.readIndex = (this.readIndex + 1) % this.capacity;
        this.available--;
        this.phase = 0;
        this.droppedSamples++;
      }
      this.buffer[this.writeIndex] = samples[index];
      this.writeIndex = (this.writeIndex + 1) % this.capacity;
      this.available++;
    }
    if (!this.started && this.available >= this.targetSamples) this.started = true;
  }

  postStats() {
    if (currentFrame - this.lastStatsFrame < sampleRate / 4) return;
    this.lastStatsFrame = currentFrame;
    this.port.postMessage({
      type: "stats",
      bufferedSamples: this.available,
      bufferedMs: this.available * 1000 / this.inputSampleRate,
      underruns: this.underruns,
      droppedSamples: this.droppedSamples,
    });
  }

  process(_inputs, outputs) {
    const output = outputs[0]?.[0];
    if (!output) return true;
    output.fill(0);
    if (!this.started) {
      if (this.available >= this.targetSamples) this.started = true;
      else {
        this.postStats();
        return true;
      }
    }

    const step = this.inputSampleRate / sampleRate;
    for (let index = 0; index < output.length; index++) {
      if (this.available < 2) {
        this.underruns++;
        this.started = false;
        this.phase = 0;
        break;
      }
      const current = this.buffer[this.readIndex];
      const next = this.buffer[(this.readIndex + 1) % this.capacity];
      output[index] = current + (next - current) * this.phase;
      this.phase += step;
      while (this.phase >= 1 && this.available > 0) {
        this.phase -= 1;
        this.readIndex = (this.readIndex + 1) % this.capacity;
        this.available--;
      }
    }
    this.postStats();
    return true;
  }
}

registerProcessor("pcm16-stream-player", PCM16StreamPlayer);
