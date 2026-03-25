class ContinuousCaptureProcessor extends AudioWorkletProcessor {
  process(inputs, outputs, parameters) {
    const input = inputs[0];
    if (input && input.length > 0 && input[0]) {
      const channelData = input[0];
      const copy = new Float32Array(channelData);
      this.port.postMessage(copy);
    }
    return true;
  }
}

registerProcessor('continuous-capture-processor', ContinuousCaptureProcessor);