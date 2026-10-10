import {onOff} from 'zigbee-herdsman-converters/lib/modernExtend';
import * as exposes from 'zigbee-herdsman-converters/lib/exposes';

const melodyCluster = {
    ID: 0xFC00,
    name: 'melodyCluster',
    attributes: {
        melody: {ID: 0x0000, name: 'melody', type: 0x42, write: true},
        last_error: {ID: 0x0001, name: 'last_error', type: 0x20},
    },
    commands: {
        play: {ID: 0x00, name: 'play', parameters: []},
    },
    commandsResponse: {},
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
        if (typeof value !== 'string' || value.length === 0 || value.length > 120) {
            throw new Error('melody must be a 1..120 character string');
        }
        await entity.write('melodyCluster', {melody: value}, meta.options);
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
            .withDescription('Space-separated notes/rests, e.g. C5:150 R:50 E5:300'),
        exposes.presets.enum('melody_play', exposes.access.SET, ['PLAY'])
            .withDescription('Play the currently configured melody'),
        exposes.presets.numeric('melody_error', exposes.access.STATE_GET)
            .withValueMin(0).withValueMax(6)
            .withDescription('Last melody write error (0 means valid)'),
    ],
    configure: async (device) => {
        device.addCustomCluster('melodyCluster', melodyCluster);
    },
};
