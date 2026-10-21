// expose bounded wire helpers to browsers and Node tests.
(function (root, factory) {
    "use strict";
    const api = factory();
    if (typeof module === "object" && module.exports) module.exports = api;
    else root.RelayProtocol = api;
}(typeof globalThis === "object" ? globalThis : this, function () {
    "use strict";
    const alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const topicPattern = /^[A-Za-z0-9][A-Za-z0-9._/-]{0,31}$/;

    // stop before using malformed metadata or allocating unbounded buffers.
    function requireValue(condition, message) {
        if (!condition) throw new Error(message);
    }

    // restrict timestamps and sequence values to the firmware's unsigned field.
    function isSequence(value) {
        return Number.isInteger(value) && value >= 0 && value <= 0xffffffff;
    }

    // decode canonical Base64, including zero padding bits.
    function decodeBase64(text) {
        requireValue(typeof text === "string" && text.length <= 252 &&
            /^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(text),
            "Invalid Base64 packet.");
        const bytes = [];
        for (let i = 0; i < text.length; i += 4) {
            const a = alphabet.indexOf(text[i]);
            const b = alphabet.indexOf(text[i + 1]);
            const c = text[i + 2] === "=" ? 0 : alphabet.indexOf(text[i + 2]);
            const d = text[i + 3] === "=" ? 0 : alphabet.indexOf(text[i + 3]);
            requireValue((text[i + 2] !== "=" || (b & 15) === 0) &&
                (text[i + 3] !== "=" || text[i + 2] === "=" || (c & 3) === 0),
                "Noncanonical Base64 packet.");
            bytes.push((a << 2) | (b >> 4));
            if (text[i + 2] !== "=") bytes.push(((b & 15) << 4) | (c >> 2));
            if (text[i + 3] !== "=") bytes.push(((c & 3) << 6) | d);
        }
        return Uint8Array.from(bytes);
    }

    // match the firmware's CRC32 over all bytes preceding the checksum.
    function crc32(bytes) {
        let crc = 0xffffffff;
        for (const byte of bytes) {
            crc ^= byte;
            for (let bit = 0; bit < 8; ++bit) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
        }
        return (crc ^ 0xffffffff) >>> 0;
    }

    // validate XWR framing; firmware remains authoritative for payload semantics.
    function inspectPacket(packet) {
        requireValue(packet && typeof packet.body === "string" &&
            packet.body.length <= 256 && packet.body.startsWith("XW1:"),
            "Invalid XWR packet body.");
        const bytes = decodeBase64(packet.body.slice(4));
        requireValue(bytes.length >= 17 && bytes.length <= 189, "Invalid XWR packet size.");
        const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        const codec = bytes[3];
        const sequence = view.getUint32(4, true);
        const topicLength = bytes[8];
        const payloadLength = view.getUint16(9, true);
        requireValue(bytes[0] === 0x58 && bytes[1] === 0x57 && bytes[2] === 1 &&
            (codec === 0 || codec === 1), "Unsupported XWR version or codec.");
        requireValue(topicLength >= 1 && topicLength <= 32 && payloadLength >= 1 &&
            11 + topicLength + payloadLength + 4 === bytes.length, "Invalid XWR field lengths.");
        const topic = String.fromCharCode.apply(null, bytes.subarray(11, 11 + topicLength));
        requireValue(topicPattern.test(topic) && (topic === "weather") === (codec === 1) &&
            (codec !== 1 || payloadLength === 118), "Invalid XWR topic or payload size.");
        requireValue(crc32(bytes.subarray(0, -4)) === view.getUint32(bytes.length - 4, true),
            "XWR checksum failed.");
        requireValue(packet.topic === topic && isSequence(packet.sequence) &&
            packet.sequence === sequence, "Snapshot metadata does not match XWR packet.");
        return {topic: topic, sequence: sequence, body: packet.body};
    }

    // reject stale snapshots against both server and phone time.
    function inspectSnapshot(snapshot, nowSeconds) {
        requireValue(snapshot && snapshot.version === 1 && isSequence(snapshot.server_utc) &&
            isSequence(snapshot.fetched_utc) && Number.isFinite(nowSeconds),
            "Invalid snapshot metadata.");
        requireValue(Math.abs(nowSeconds - snapshot.server_utc) <= 300,
            "Server and phone clocks differ, or the response is stale.");
        requireValue(snapshot.fetched_utc <= snapshot.server_utc + 300 &&
            snapshot.server_utc - snapshot.fetched_utc <= 900,
            "The PC snapshot is stale. Wait for a successful source refresh.");
        requireValue(Array.isArray(snapshot.packets) && snapshot.packets.length >= 1 &&
            snapshot.packets.length <= 8, "Invalid snapshot packet count.");
        const topics = new Set();
        return snapshot.packets.map(function (item) {
            const packet = inspectPacket(item);
            requireValue(!topics.has(packet.topic), "Duplicate snapshot topic.");
            requireValue(snapshot.server_utc - packet.sequence <= 900 &&
                packet.sequence - snapshot.server_utc <= 300, "Packet sequence is stale or in the future.");
            topics.add(packet.topic);
            return packet;
        });
    }

    // split one LF-terminated ASCII packet into bounded acknowledged writes.
    function chunkPacket(packet, size) {
        inspectPacket(packet);
        requireValue(Number.isInteger(size) && size >= 1 && size <= 20, "Invalid BLE chunk size.");
        const line = packet.body + "\n";
        const chunks = [];
        for (let start = 0; start < line.length; start += size) {
            chunks.push(Uint8Array.from(line.slice(start, start + size), function (ch) { return ch.charCodeAt(0); }));
        }
        return chunks;
    }

    // reconstruct ASCII JSON lines without accepting overflow or hidden controls.
    function LineFramer(limit) {
        this.limit = limit || 1024;
        this.partial = "";
    }

    // preserve fragments until LF; callers close a truncated stream on timeout.
    LineFramer.prototype.push = function (input) {
        const bytes = input instanceof DataView ?
            new Uint8Array(input.buffer, input.byteOffset, input.byteLength) : input;
        const lines = [];
        for (const byte of bytes) {
            requireValue(byte === 10 || byte === 13 || (byte >= 32 && byte <= 126),
                "Non-ASCII BLE response.");
            if (byte === 10) {
                const line = this.partial.endsWith("\r") ? this.partial.slice(0, -1) : this.partial;
                requireValue(line.length > 0 && !line.includes("\r"), "Invalid BLE line ending.");
                lines.push(line);
                this.partial = "";
            } else {
                requireValue(this.partial.length < this.limit, "BLE response exceeded its limit.");
                this.partial += String.fromCharCode(byte);
            }
        }
        return lines;
    };

    // accept only the watch's versioned refresh request or acknowledgement.
    function parseControlLine(line) {
        let message;
        try { message = JSON.parse(line); }
        catch (_) { throw new Error("Malformed BLE response."); }
        requireValue(message && message.v === 1, "Unsupported BLE response version.");
        if (message.type === "poll") {
            requireValue(message.interval === 900, "Unsupported watch polling interval.");
            return message;
        }
        requireValue(message.type === "ack" && typeof message.ok === "boolean" &&
            typeof message.status === "string" && /^[a-z_]{1,40}$/.test(message.status),
            "Invalid watch acknowledgement.");
        return message;
    }

    // confirm application acceptance only for the exact topic and sequence.
    function acknowledgementMatches(message, packet) {
        return message && message.v === 1 && message.type === "ack" &&
            message.for === "data" && message.topic === packet.topic &&
            isSequence(message.seq) && message.seq === packet.sequence;
    }

    // never treat a successful GATT write as application acceptance.
    function acknowledgementAccepted(message, packet) {
        return acknowledgementMatches(message, packet) && message.ok === true &&
            (message.status === "applied" || message.status === "duplicate");
    }

    return {crc32: crc32, inspectPacket: inspectPacket, inspectSnapshot: inspectSnapshot,
        chunkPacket: chunkPacket, LineFramer: LineFramer, parseControlLine: parseControlLine,
        acknowledgementMatches: acknowledgementMatches, acknowledgementAccepted: acknowledgementAccepted};
}));