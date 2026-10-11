import {onOff} from 'zigbee-herdsman-converters/lib/modernExtend';
import * as exposes from 'zigbee-herdsman-converters/lib/exposes';

const melodyCluster = {
    ID: 0xFC00,
    name: 'melodyCluster',
    attributes: {
        melody: {ID: 0x0000, name: 'melody', type: 0x42, write: true},
        last_error: {ID: 0x0001, name: 'last_error', type: 0x20},
        melody_ctrl: {ID: 0x0002, name: 'melody_ctrl', type: 0x20, write: true},
        melody_chunk: {ID: 0x0003, name: 'melody_chunk', type: 0x42, write: true},
    },
    commands: {
        play: {ID: 0x00, name: 'play', parameters: []},
    },
    commandsResponse: {},
};

const MELODY_MAX = 300;
// Keep chunks well below the Zigbee APS payload limit: 79 chars plus ZCL/APS/NWK
// overhead was rejected by the coordinator with ApsDataRequest InvalidValue.
const MELODY_CHUNK_MAX = 48;
const melodyErrorNames = {
    0: 'success',
    1: 'invalid length',
    2: 'syntax error',
    3: 'too many or no tokens',
    4: 'duration out of range',
    5: 'invalid note',
    6: 'total duration exceeded',
    7: 'upload sequence error',
    8: 'unknown upload operation',
};

const fzMelody = {
    cluster: 'melodyCluster',
    type: ['attributeReport', 'readResponse'],
    convert: (_model, msg) => {
        const result = {};
        if (msg.data.melody !== undefined) result.melody = msg.data.melody;
        if (msg.data.last_error !== undefined) result.melody_error = msg.data.last_error;
        return result;
    },
};

const tzMelody = {
    key: ['melody'],
    async convertSet(entity, _key, value, meta) {
        if (typeof value !== 'string' || value.length > MELODY_MAX || !/^[\x20-\x7e]*$/.test(value)) {
            throw new Error(`melody must be an ASCII string of 0..${MELODY_MAX} characters`);
        }

        const chunks = [];
        for (let offset = 0; offset < value.length; offset += MELODY_CHUNK_MAX) {
            chunks.push(value.slice(offset, offset + MELODY_CHUNK_MAX));
        }

        try {
            await entity.write('melodyCluster', {melody_ctrl: 1}, meta.options);
            for (const chunk of chunks) {
                await entity.write('melodyCluster', {melody_chunk: chunk}, meta.options);
            }
            await entity.write('melodyCluster', {melody_ctrl: 2}, meta.options);
        } catch (cause) {
            // ABORT preserves the device's last_error while discarding any open staging data.
            try {
                await entity.write('melodyCluster', {melody_ctrl: 3}, meta.options);
            } catch {
                // The upload may have already failed or the device may be unreachable.
            }
            try {
                await entity.read('melodyCluster', ['last_error'], meta.options);
            } catch {
                // Keep the original write error if the diagnostic read also fails.
            }
            const codes = Object.entries(melodyErrorNames).map(([code, name]) => `${code}=${name}`).join(', ');
            throw new Error(`melody upload failed; check melody_error (${codes}): ${cause?.message ?? cause}`);
        }

        // Read back the status so parser/session failures are detected even if the stack
        // accepted the commit write, and to clear an old nonzero melody_error state.
        let uploadError;
        try {
            const status = await entity.read('melodyCluster', ['last_error'], meta.options);
            uploadError = status?.last_error;
        } catch {
            // The attribute read is diagnostic; preserve the acknowledged write result.
        }
        if (Number.isInteger(uploadError) && uploadError !== 0) {
            const description = melodyErrorNames[uploadError] ?? 'unknown error';
            throw new Error(`melody upload rejected (melody_error ${uploadError}: ${description})`);
        }

        return {state: {melody: value}};
    },
    async convertGet(entity, _key, meta) {
        await entity.read('melodyCluster', ['melody'], meta.options);
    },
};

const tzMelodyPlay = {
    key: ['melody_play'],
    async convertSet(entity, _key, value, meta) {
        if (value !== 'PLAY') throw new Error('melody_play only accepts PLAY');
        await entity.command('melodyCluster', 'play', {}, meta.options);
    },
};

const tzMelodyError = {
    key: ['melody_error'],
    async convertGet(entity, _key, meta) {
        await entity.read('melodyCluster', ['last_error'], meta.options);
    },
};

export default {
    fingerprint: [{modelID: 'esp32c6', manufacturerName: 'ESPRESSIF'}],
    model: 'esp32c6-melody',
    vendor: 'ESPRESSIF',
    description: 'ESP32-C6 multi-sensor with configurable buzzer melodies',
    endpoint: () => ({default: 10}),
    extend: [onOff()],
    fromZigbee: [fzMelody],
    toZigbee: [tzMelody, tzMelodyPlay, tzMelodyError],
    exposes: [
        exposes.presets.text('melody', exposes.access.ALL)
            .withDescription('Up to 300 characters, uploaded in sequential chunks of up to 48 characters; empty clears to default'),
        exposes.presets.enum('melody_play', exposes.access.SET, ['PLAY'])
            .withDescription('Play the active melody, or the default if none is set'),
        exposes.presets.numeric('melody_error', exposes.access.STATE_GET)
            .withValueMin(0).withValueMax(8)
            .withDescription('Last melody upload error (0 means success; 1-8 describe upload/validation failures)'),
    ],
    configure: async (device) => {
        device.addCustomCluster('melodyCluster', melodyCluster);
    },
};
