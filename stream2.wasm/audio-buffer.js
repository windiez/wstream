/**
 * AudioChunkBuffer - ring buffer for Float32 audio samples.
 *
 * Drop policy: when appending would exceed capacity, the OLDEST samples are
 * dropped (never the newest). This matches a live-audio pipeline where the
 * most recent audio is always more important than stale history.
 *
 * Compatible with both browser <script> tags and Node.js require().
 */

(function (root, factory) {
  if (typeof module !== "undefined" && module.exports) {
    // Node.js / CommonJS
    module.exports = factory();
  } else {
    // Browser global
    root.AudioChunkBuffer = factory();
  }
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";

  class AudioChunkBuffer {
    /**
     * @param {number} maxSamples - positive integer capacity
     */
    constructor(maxSamples) {
      if (!Number.isInteger(maxSamples) || maxSamples <= 0) {
        throw new RangeError("maxSamples must be a positive integer");
      }
      this._buf = new Float32Array(maxSamples);
      this._cap = maxSamples;
      this._head = 0; // index of the oldest sample
      this._len = 0;  // number of valid samples currently stored
    }

    /** Number of samples currently stored. */
    get length() {
      return this._len;
    }

    /** Maximum number of samples the buffer can hold. */
    get capacity() {
      return this._cap;
    }

    /**
     * Append samples. If the new data would exceed capacity, the oldest
     * samples are silently discarded to make room.
     *
     * @param {Float32Array} chunk
     */
    append(chunk) {
      if (chunk.length === 0) return;

      const cap = this._cap;

      if (chunk.length >= cap) {
        // The new chunk is at least as large as the entire buffer:
        // keep only the last 'cap' samples from the chunk.
        const start = chunk.length - cap;
        this._buf.set(chunk.subarray(start));
        this._head = 0;
        this._len = cap;
        return;
      }

      const incoming = chunk.length;
      const available = cap - this._len; // free slots

      if (incoming > available) {
        // Drop oldest (incoming - available) samples to make room.
        const toDrop = incoming - available;
        this._head = (this._head + toDrop) % cap;
        this._len -= toDrop;
      }

      // Write samples into the ring buffer, wrapping around as needed.
      const tail = (this._head + this._len) % cap;
      const firstPart = Math.min(incoming, cap - tail);
      this._buf.set(chunk.subarray(0, firstPart), tail);
      if (firstPart < incoming) {
        this._buf.set(chunk.subarray(firstPart), 0);
      }
      this._len += incoming;
    }

    /**
     * Remove and return the first n samples as a Float32Array.
     * If n > available, returns all available samples.
     * If the buffer is empty, returns an empty Float32Array.
     *
     * @param {number} n
     * @returns {Float32Array}
     */
    consume(n) {
      const count = Math.min(n, this._len);
      if (count === 0) return new Float32Array(0);

      const cap = this._cap;
      const out = new Float32Array(count);
      const firstPart = Math.min(count, cap - this._head);
      out.set(this._buf.subarray(this._head, this._head + firstPart));
      if (firstPart < count) {
        out.set(this._buf.subarray(0, count - firstPart), firstPart);
      }

      this._head = (this._head + count) % cap;
      this._len -= count;
      return out;
    }

    /**
     * Empty the buffer without changing capacity.
     */
    clear() {
      this._head = 0;
      this._len = 0;
    }

    /**
     * Return all stored samples as a contiguous Float32Array, oldest first.
     *
     * @returns {Float32Array}
     */
    toFloat32Array() {
      if (this._len === 0) return new Float32Array(0);

      const cap = this._cap;
      const out = new Float32Array(this._len);
      const firstPart = Math.min(this._len, cap - this._head);
      out.set(this._buf.subarray(this._head, this._head + firstPart));
      if (firstPart < this._len) {
        out.set(this._buf.subarray(0, this._len - firstPart), firstPart);
      }
      return out;
    }
  }

  return AudioChunkBuffer;
});
