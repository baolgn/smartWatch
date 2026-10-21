// keep browser state private and credentials in memory.
(function () {
    "use strict";
    const protocol = window.RelayProtocol;
    const nus = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
    const rxUuid = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
    const txUuid = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";
    const elements = {};
    ["token_form", "access_token", "connect", "choose_watch", "disconnect", "refresh",
        "status", "deliveries", "snapshot_time"].forEach(function (id) {
        elements[id] = document.getElementById(id);
    });
    let token = "";
    let generation = 0;
    let connection = null;
    let connecting = false;
    let rememberedDevice = null;
    let deviceLookupGeneration = 0;
    let deviceLookupTimer = null;
    let cancelLookupWait = null;
    let nativeWrite = null;
    let lookingUpDevice = false;
    let ambiguousDevices = false;
    let fetchController = null;
    let cycle = null;
    let refreshQueued = false;
    let pendingAck = null;
    let synchronization = null;
    let lastProbeNonce = null;
    let fragmentTimer = null;
    let framer = new protocol.LineFramer(1024);
    const accepted = new Map();
    const rows = new Map();

    // expose connection state without rendering untrusted HTML.
    function status(text, error) {
        elements.status.textContent = text;
        elements.status.dataset.error = error ? "true" : "false";
    }

    // require HTTPS on the phone and a Web Bluetooth browser.
    function supported() {
        if (!window.isSecureContext) return "Open the PC's HTTPS address. Bluetooth requires a secure page.";
        if (!navigator.bluetooth) return "Open this page in Bluefy on iPhone, or a browser with Web Bluetooth.";
        return "";
    }

    // preserve native Bluetooth details without credentials or unbounded output.
    function bluetoothError(error) {
        const clean = function (value, limit) {
            let text = typeof value === "string" ? value : "";
            if (token) text = text.split(token).join("[redacted]");
            return text.replace(/[\x00-\x1f\x7f]/g, " ").trim().slice(0, limit);
        };
        const name = clean(error && error.name, 48) || "Error";
        const message = clean(typeof error === "string" ? error : error && error.message, 180);
        return name + (message ? ": " + message : ": Bluetooth operation failed.");
    }

    // cleanup failures must not replace the error that ended a session.
    function closeGatt(server) {
        try {
            if (server && server.connected) server.disconnect();
            return true;
        } catch (_) {
            return false;
        }
    }

    // reflect user actions and avoid overlapping connection requests.
    function buttons() {
        const unavailable = !token || !!supported() || connecting || !!connection;
        elements.connect.textContent = rememberedDevice ? "Reconnect watch" : "Connect watch";
        elements.connect.disabled = unavailable || lookingUpDevice || (ambiguousDevices && !rememberedDevice);
        elements.choose_watch.disabled = unavailable;
        elements.disconnect.disabled = !connection && !connecting;
        elements.refresh.disabled = !connection || !!cycle;
    }

    // invalidate lookup callbacks without revoking saved Bluetooth access.
    function cancelDeviceLookup() {
        ++deviceLookupGeneration;
        clearTimeout(deviceLookupTimer);
        deviceLookupTimer = null;
        if (cancelLookupWait) cancelLookupWait();
        cancelLookupWait = null;
        lookingUpDevice = false;
    }

    // restore one previously permitted watch without scanning or connecting.
    function restoreDevice() {
        if (rememberedDevice || connection || connecting || supported() ||
            typeof navigator.bluetooth.getDevices !== "function") return;
        cancelDeviceLookup();
        const lookup = deviceLookupGeneration;
        const epoch = generation;
        lookingUpDevice = true;
        ambiguousDevices = false;
        buttons();
        let timeout;
        const deadline = new Promise(function (_, reject) {
            cancelLookupWait = function () { reject(new Error("Saved watch lookup cancelled.")); };
            timeout = setTimeout(function () {
                reject(new Error("Saved watch lookup timed out."));
            }, 3000);
            deviceLookupTimer = timeout;
        });
        const operation = Promise.resolve().then(function () {
            if (lookup !== deviceLookupGeneration || epoch !== generation) return [];
            return navigator.bluetooth.getDevices();
        });
        Promise.race([operation, deadline]).then(function (devices) {
            if (lookup !== deviceLookupGeneration || epoch !== generation) return;
            if (!Array.isArray(devices)) throw new Error("Saved watch lookup returned an invalid list.");
            const matches = devices.filter(function (device) {
                return device && device.name === "XIAO Watch" && device.gatt &&
                    typeof device.gatt.connect === "function";
            });
            if (matches.length === 1) {
                rememberedDevice = matches[0];
                if (token) status("Saved watch available. Select Reconnect watch.");
            } else if (matches.length > 1) {
                ambiguousDevices = true;
                if (token) status("More than one watch has access. Select Choose watch.");
            }
        }).catch(function (error) {
            if (lookup === deviceLookupGeneration && epoch === generation && token) {
                status("Saved watch lookup unavailable. Select Choose watch. " + bluetoothError(error), true);
            }
        }).finally(function () {
            clearTimeout(timeout);
            if (lookup !== deviceLookupGeneration || epoch !== generation) return;
            deviceLookupTimer = null;
            cancelLookupWait = null;
            lookingUpDevice = false;
            buttons();
        });
    }

    // prevent callbacks from an old connection from modifying the new session.
    function ensureSession(epoch) {
        if (epoch !== generation || !connection || !connection.server.connected) {
            throw new Error("The watch connection ended.");
        }
    }

    // maintain one acknowledgement result per topic in the current connection.
    function delivery(topic, result, time) {
        let row = rows.get(topic);
        if (!row) {
            const empty = document.getElementById("empty_row");
            if (empty) empty.remove();
            row = document.createElement("tr");
            for (let i = 0; i < 3; ++i) row.appendChild(document.createElement("td"));
            row.children[0].textContent = topic;
            elements.deliveries.appendChild(row);
            rows.set(topic, row);
        }
        row.children[1].textContent = result;
        if (time) row.children[2].textContent = time.toLocaleString();
        else if (!row.children[2].textContent) row.children[2].textContent = "—";
    }

    // cancel waits and detach callbacks before disconnecting GATT.
    function endSession(message, error) {
        ++generation;
        cancelDeviceLookup();
        connecting = false;
        if (fetchController) fetchController.abort();
        fetchController = null;
        if (pendingAck) pendingAck.finish(new Error("The relay session ended."));
        if (synchronization) synchronization.finish(new Error("The relay session ended."));
        clearTimeout(fragmentTimer);
        fragmentTimer = null;
        framer = new protocol.LineFramer(1024);
        const previous = connection;
        connection = null;
        refreshQueued = false;
        accepted.clear();
        rows.forEach(function (row) { row.children[1].textContent = "Disconnected"; });
        let cleaned = true;
        if (previous) {
            try {
                previous.device.removeEventListener("gattserverdisconnected", previous.onDisconnect);
            } catch (_) {
                cleaned = false;
            }
            try {
                if (previous.tx) previous.tx.removeEventListener("characteristicvaluechanged", previous.onValue);
            } catch (_) {
                cleaned = false;
            }
            if (!closeGatt(previous.device.gatt)) cleaned = false;
        }
        status(message + (cleaned ? "" : " Close Bluefy to release Bluetooth."), error || !cleaned);
        buttons();
    }

    // time out GATT calls while consuming late settlements safely.
    async function bounded(operation, milliseconds, epoch) {
        let timer;
        try {
            const value = await Promise.race([operation, new Promise(function (_, reject) {
                timer = setTimeout(function () { reject(new Error("Bluetooth operation timed out.")); }, milliseconds);
            })]);
            ensureSession(epoch);
            return value;
        } finally {
            clearTimeout(timer);
        }
    }

    // reserve the matching response before the final write can trigger it.
    function waitForAck(packet, epoch) {
        let resolveWait;
        let rejectWait;
        let timer;
        const promise = new Promise(function (resolve, reject) {
            resolveWait = resolve;
            rejectWait = reject;
        });
        promise.catch(function () {});
        const waiter = {packet: packet, epoch: epoch, finish: function (error, reply) {
            if (pendingAck !== waiter) return;
            clearTimeout(timer);
            pendingAck = null;
            if (error) rejectWait(error);
            else resolveWait(reply);
        }};
        pendingAck = waiter;
        timer = setTimeout(function () {
            waiter.finish(new Error("The watch did not acknowledge the update. Reconnect to retry."));
        }, 15000);
        return promise;
    }

    // reserve a fresh, nonmutating probe before restored TX subscriptions can reply.
    function prepareSynchronization(epoch) {
        if (!window.crypto || typeof window.crypto.getRandomValues !== "function") {
            throw new Error("The browser cannot create a secure connection probe.");
        }
        const nonce = new Uint32Array(1);
        for (let attempt = 0; attempt < 4; ++attempt) {
            window.crypto.getRandomValues(nonce);
            if (nonce[0] !== lastProbeNonce) break;
        }
        if (nonce[0] === lastProbeNonce) throw new Error("The browser repeated the connection probe.");
        lastProbeNonce = nonce[0];
        let resolveWait;
        let rejectWait;
        let timer;
        const promise = new Promise(function (resolve, reject) {
            resolveWait = resolve;
            rejectWait = reject;
        });
        promise.catch(function () {});
        const probe = {epoch: epoch, nonce: nonce[0], partial: "", bytes: 0, expectReply: false,
            started: Date.now(), promise: promise, finish: function (error) {
                if (synchronization !== probe) return;
                clearTimeout(timer);
                synchronization = null;
                framer = new protocol.LineFramer(1024);
                if (error) rejectWait(error);
                else resolveWait();
            }};
        synchronization = probe;
        timer = setTimeout(function () {
            probe.finish(new Error("The watch connection probe timed out. Reconnect to retry."));
        }, 15000);
        return probe;
    }

    // bound synchronization even when browser timers pause.
    function checkProbeDeadline(probe) {
        const elapsed = Date.now() - probe.started;
        if (elapsed < 0 || elapsed > 15000) throw new Error("The watch connection probe expired.");
    }

    // drain old ACK fragments only until the exact fresh probe acknowledgement.
    function consumeSynchronization(bytes) {
        const probe = synchronization;
        checkProbeDeadline(probe);
        probe.bytes += bytes.length;
        if (probe.bytes > 4096) throw new Error("Too much pending watch data during connection.");
        for (let index = 0; index < bytes.length; ++index) {
            const byte = bytes[index];
            if (byte !== 10 && byte !== 13 && (byte < 32 || byte > 126)) {
                throw new Error("Non-ASCII watch data during connection.");
            }
            if (byte !== 10) {
                if (probe.partial.length >= 1024) throw new Error("Pending watch response exceeded its limit.");
                probe.partial += String.fromCharCode(byte);
                continue;
            }
            let reply = null;
            try { reply = JSON.parse(probe.partial); } catch (_) {}
            probe.partial = "";
            if (probe.expectReply && reply && reply.v === 1 && reply.type === "ack" &&
                reply.for === "unknown" && reply.seq === probe.nonce && reply.ok === false &&
                reply.status === "invalid_message") {
                probe.finish(null);
                return index + 1;
            }
        }
        return bytes.length;
    }

    // serialize every acknowledged write through one implementation.
    async function writeBytes(bytes, epoch) {
        ensureSession(epoch);
        const rx = connection.rx;
        if (nativeWrite) throw new Error("A previous Bluetooth write is still pending.");
        const operation = typeof rx.writeValueWithResponse === "function" ?
            rx.writeValueWithResponse(bytes) : rx.writeValue(bytes);
        const write = {promise: Promise.resolve(operation)};
        nativeWrite = write;
        // track native completion even after the application's deadline or cancellation.
        const clear = function () { if (nativeWrite === write) nativeWrite = null; };
        write.promise.then(clear, clear);
        await bounded(write.promise, 5000, epoch);
    }

    // keep a late native write from crossing the next connection's probe.
    async function finishPreviousWrite(epoch) {
        if (!nativeWrite) return;
        const previous = nativeWrite;
        let timer;
        try {
            await Promise.race([previous.promise.catch(function () {}), new Promise(function (_, reject) {
                timer = setTimeout(function () {
                    reject(new Error("A previous Bluetooth write has not finished. Close and reopen Bluefy."));
                }, 5000);
            })]);
            if (epoch !== generation) throw new Error("The relay session ended.");
        } finally {
            clearTimeout(timer);
        }
    }

    // reset unfinished RX input and drain old TX replies without changing watch data.
    async function synchronize(probe, epoch) {
        const line = JSON.stringify({v: 1, type: "probe", seq: probe.nonce}) + "\n";
        const chunks = [Uint8Array.of(0, 10)];
        for (let start = 0; start < line.length; start += 20) {
            chunks.push(Uint8Array.from(line.slice(start, start + 20), function (ch) { return ch.charCodeAt(0); }));
        }
        for (let index = 0; index < chunks.length; ++index) {
            ensureSession(epoch);
            checkProbeDeadline(probe);
            if (synchronization !== probe) {
                await probe.promise;
                throw new Error("The watch connection probe ended.");
            }
            if (document.hidden) throw new Error("Keep the relay page visible while connecting.");
            if (index === chunks.length - 1) probe.expectReply = true;
            await writeBytes(chunks[index], epoch);
        }
        await probe.promise;
        ensureSession(epoch);
        checkProbeDeadline(probe);
    }

    // process complete replies and close streams with incomplete trailing bytes.
    function receive(event, epoch) {
        if (epoch !== generation) return;
        try {
            const value = event.target.value;
            const bytes = value instanceof DataView ?
                new Uint8Array(value.buffer, value.byteOffset, value.byteLength) : value;
            const offset = synchronization ? consumeSynchronization(bytes) : 0;
            if (synchronization) return;
            const lines = framer.push(bytes.subarray(offset));
            for (const line of lines) {
                const message = protocol.parseControlLine(line);
                if (message.type === "poll") {
                    requestUpdate();
                } else if (pendingAck && protocol.acknowledgementMatches(message, pendingAck.packet)) {
                    pendingAck.finish(null, message);
                } else if (message.ok === false && message.seq === null) {
                    throw new Error("The watch rejected the BLE stream: " + message.status + ".");
                }
            }
            if (!framer.partial) {
                clearTimeout(fragmentTimer);
                fragmentTimer = null;
            } else if (!fragmentTimer) {
                fragmentTimer = setTimeout(function () {
                    if (epoch === generation) endSession("The watch response was truncated. Reconnect to retry.", true);
                }, 5000);
            }
        } catch (error) {
            endSession(error.message, true);
        }
    }

    // bound response size before JSON parsing.
    async function readSnapshot(response) {
        if (!response.body || !response.body.getReader) {
            const text = await response.text();
            if (text.length > 16384) throw new Error("The server response is too large.");
            return JSON.parse(text);
        }
        const reader = response.body.getReader();
        const chunks = [];
        let length = 0;
        try {
            while (true) {
                const next = await reader.read();
                if (next.done) break;
                length += next.value.byteLength;
                if (length > 16384) throw new Error("The server response is too large.");
                chunks.push(next.value);
            }
        } finally {
            await reader.cancel().catch(function () {});
            reader.releaseLock();
        }
        const bytes = new Uint8Array(length);
        let offset = 0;
        for (const chunk of chunks) {
            bytes.set(chunk, offset);
            offset += chunk.byteLength;
        }
        return JSON.parse(new TextDecoder("utf-8", {fatal: true}).decode(bytes));
    }

    // use a same-origin request without URL credentials or cached snapshots.
    async function fetchSnapshot(epoch) {
        const controller = new AbortController();
        fetchController = controller;
        const started = performance.now();
        const timer = setTimeout(function () { controller.abort(); }, 10000);
        try {
            const response = await fetch("/api/snapshot", {
                method: "GET", headers: {Authorization: "Bearer " + token, Accept: "application/json"},
                cache: "no-store", credentials: "omit", redirect: "error", signal: controller.signal
            });
            ensureSession(epoch);
            if (response.status === 401) throw new Error("The server rejected the access token.");
            if (response.status === 503) throw new Error("The PC has no fresh snapshot. Check its source connection.");
            if (!response.ok) throw new Error("The PC snapshot request failed.");
            const snapshot = await readSnapshot(response);
            ensureSession(epoch);
            if (performance.now() - started > 10000) throw new Error("The server response arrived too late.");
            const packets = protocol.inspectSnapshot(snapshot, Date.now() / 1000);
            elements.snapshot_time.textContent = "PC fetched data: " + new Date(snapshot.fetched_utc * 1000).toLocaleString();
            return packets;
        } finally {
            clearTimeout(timer);
            if (fetchController === controller) fetchController = null;
        }
    }

    // send one packet serially and require its application-level acknowledgement.
    async function sendPacket(packet, epoch) {
        ensureSession(epoch);
        const prior = accepted.get(packet.topic);
        if (prior && packet.sequence === prior.sequence && packet.body === prior.body) return;
        if (prior && packet.sequence <= prior.sequence) throw new Error("The PC supplied conflicting or older data.");
        if (document.hidden) return;
        // recheck age after earlier packets or a suspended page.
        const age = Date.now() / 1000 - packet.sequence;
        if (age > 900 || age < -300) throw new Error("The packet expired. Wait for the PC to refresh its data.");
        delivery(packet.topic, "Sending…");
        const ack = waitForAck(packet, epoch);
        const started = Date.now();
        // reject resumed transfers whose browser timers were suspended.
        function checkDeadline() {
            const elapsed = Date.now() - started;
            if (elapsed < 0 || elapsed > 15000) throw new Error("Bluetooth delivery expired. Reconnect to retry.");
        }
        try {
            for (const bytes of protocol.chunkPacket(packet, 20)) {
                ensureSession(epoch);
                checkDeadline();
                if (document.hidden) throw new Error("Delivery paused while the page was hidden. Reconnect to retry.");
                await writeBytes(bytes, epoch);
            }
            const reply = await ack;
            ensureSession(epoch);
            checkDeadline();
            if (!protocol.acknowledgementAccepted(reply, packet)) {
                delivery(packet.topic, "Rejected: " + reply.status);
                throw new Error("The watch rejected " + packet.topic + ": " + reply.status + ".");
            }
            accepted.set(packet.topic, packet);
            delivery(packet.topic, reply.status === "duplicate" ? "Already accepted" : "Applied", new Date());
        } catch (error) {
            if (epoch === generation) endSession(error.message || "Bluetooth delivery failed.", true);
            throw error;
        }
    }

    // serialize fetch/send cycles even when watch polls arrive during delivery.
    function requestUpdate() {
        if (!connection || connecting || document.hidden || !token) return;
        if (cycle) {
            refreshQueued = true;
            return;
        }
        const epoch = generation;
        const running = (async function () {
            try {
                status("Checking the PC's latest data.");
                const packets = await fetchSnapshot(epoch);
                for (const packet of packets) {
                    ensureSession(epoch);
                    await sendPacket(packet, epoch);
                }
                ensureSession(epoch);
                if (!document.hidden) status("Connected. Waiting for the next PC update.");
            } catch (error) {
                if (epoch !== generation) return;
                if (error.name !== "AbortError") {
                    status(error.message || "The relay request failed.", true);
                }
            } finally {
                if (cycle === running) cycle = null;
                buttons();
                if (refreshQueued && connection && !document.hidden) {
                    refreshQueued = false;
                    setTimeout(requestUpdate, 0);
                }
            }
        }());
        cycle = running;
        buttons();
    }

    // acquire Bluetooth permission directly from the user's click.
    async function connect(chooseNew) {
        const problem = supported();
        if (problem) { status(problem, true); return; }
        if (!token || connecting || connection) return;
        if (!chooseNew && (lookingUpDevice || (ambiguousDevices && !rememberedDevice))) return;
        cancelDeviceLookup();
        ambiguousDevices = false;
        connecting = true;
        const epoch = generation;
        buttons();
        const useSaved = !chooseNew && rememberedDevice;
        status(useSaved ? "Reconnecting to the saved watch." : "Choose XIAO Watch.");
        let device;
        let stage = useSaved ? "reconnecting the saved watch" : "choosing the watch";
        try {
            // requestDevice stays in the click handler to preserve user activation.
            device = useSaved ? rememberedDevice : await navigator.bluetooth.requestDevice({
                filters: [{name: "XIAO Watch"}], optionalServices: [nus]
            });
            if (epoch !== generation) return;
            rememberedDevice = device;
            if (nativeWrite) {
                stage = "waiting for the previous Bluetooth write";
                status("Waiting for the previous Bluetooth operation.");
                await finishPreviousWrite(epoch);
                if (epoch !== generation) return;
            }
            stage = useSaved ? "reconnecting Bluetooth" : "connecting Bluetooth";
            status("Connecting Bluetooth.");
            const onDisconnect = function () {
                if (epoch !== generation) return;
                const message = connecting ? "Watch disconnected while " + stage + ". Connect again to resume." :
                    "Watch disconnected. Connect again to resume.";
                endSession(message, true);
            };
            const onValue = function (event) { receive(event, epoch); };
            connection = {device: device, server: device.gatt, rx: null, tx: null,
                onDisconnect: onDisconnect, onValue: onValue};
            device.addEventListener("gattserverdisconnected", onDisconnect);
            // close late connections unless a new session owns the same device.
            const connectingServer = device.gatt.connect().then(function (server) {
                const owned = connection && (connection.device === device ||
                    (device.id && connection.device.id === device.id));
                if (epoch !== generation && !owned) closeGatt(server);
                return server;
            });
            const server = await bounded(connectingServer, 15000, epoch);
            connection.server = server;
            stage = "discovering the UART service";
            status("Discovering the watch's UART service.");
            const service = await bounded(server.getPrimaryService(nus), 10000, epoch);
            stage = "finding the write characteristic";
            const rx = await bounded(service.getCharacteristic(rxUuid), 10000, epoch);
            stage = "checking acknowledged writes";
            if (!rx.properties.write || (typeof rx.writeValueWithResponse !== "function" &&
                typeof rx.writeValue !== "function")) {
                throw new Error("The watch does not support acknowledged BLE writes.");
            }
            connection.rx = rx;
            stage = "finding the reply characteristic";
            const tx = await bounded(service.getCharacteristic(txUuid), 10000, epoch);
            connection.tx = tx;
            tx.addEventListener("characteristicvaluechanged", onValue);
            const probe = prepareSynchronization(epoch);
            stage = "subscribing to watch replies";
            status("Subscribing to watch replies.");
            await bounded(tx.startNotifications(), 10000, epoch);
            stage = "preparing the watch connection";
            status("Preparing watch connection.");
            await synchronize(probe, epoch);
            connecting = false;
            rows.clear();
            elements.deliveries.replaceChildren();
            accepted.clear();
            buttons();
            status("Connected to XIAO Watch.");
            requestUpdate();
        } catch (error) {
            if (epoch !== generation) {
                const owned = connection && (connection.device === device ||
                    (device && device.id && connection.device.id === device.id));
                if (device && !owned) closeGatt(device.gatt);
                return;
            }
            endSession("Connection failed while " + stage + ". " + bluetoothError(error) +
                (useSaved && stage !== "waiting for the previous Bluetooth write" ?
                    " Select Choose watch to select a device again." : ""), true);
        }
    }

    // replacing credentials cancels every fetch, write and pending response.
    elements.token_form.addEventListener("submit", function (event) {
        event.preventDefault();
        const candidate = elements.access_token.value.trim();
        if (!/^[A-Za-z0-9_-]{32,128}$/.test(candidate)) {
            status("Enter the complete access token generated by the PC server.", true);
            return;
        }
        endSession("Token loaded. Connect the watch.");
        token = candidate;
        elements.access_token.value = "";
        buttons();
        const problem = supported();
        if (problem) status(problem, true);
        else restoreDevice();
    });
    elements.connect.addEventListener("click", function () { return connect(false); });
    elements.choose_watch.addEventListener("click", function () { return connect(true); });
    elements.disconnect.addEventListener("click", function () { endSession("Disconnected."); });
    elements.refresh.addEventListener("click", requestUpdate);

    // catch up on foreground return without promising background execution.
    document.addEventListener("visibilitychange", function () {
        if (document.hidden) {
            if (fetchController) fetchController.abort();
            if (pendingAck || synchronization) endSession("Delivery paused while hidden. Connect again when ready.");
            else if (connection) status("Paused while the page is hidden.");
        } else {
            requestUpdate();
        }
    });
    window.addEventListener("pageshow", requestUpdate);
    window.addEventListener("focus", requestUpdate);
    window.addEventListener("pagehide", function () { endSession("Page closed."); });
    setInterval(requestUpdate, 30000);
    const problem = supported();
    if (problem) status(problem, true);
    buttons();
    restoreDevice();
}());