// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Klima WiFi contributors
//
// Zigbee2MQTT external converter for the Klima WiFi AC Bridge.
// The converter deliberately reports only values read from the device.  A
// command is considered successful only after the AC bridge emits a matching
// frame and the next attribute report is received.

const exposes = require('zigbee-herdsman-converters/lib/exposes');
const reporting = require('zigbee-herdsman-converters/lib/reporting');

const e = exposes.presets;
const ea = exposes.access;

const THERMOSTAT_CLUSTER = 0x0201;
const FAN_CONTROL_CLUSTER = 0x0202;
const THERMOSTAT_CLUSTER_NAME = 'hvacThermostat';
const FAN_CONTROL_CLUSTER_NAME = 'hvacFanCtrl';
const KLIMA_CLUSTER = 0xFC10;
const KLIMA_CLUSTER_NAME = '64528';
const MANUFACTURER_CODE = 0x131B;

const THERMOSTAT_ATTR_LOCAL_TEMPERATURE = 0x0000;
const THERMOSTAT_ATTR_OCCUPIED_COOLING_SETPOINT = 0x0011;
const THERMOSTAT_ATTR_UNOCCUPIED_COOLING_SETPOINT = 0x0013;
const THERMOSTAT_ATTR_SYSTEM_MODE = 0x001c;
const FAN_ATTR_MODE = 0x0000;
const FAN_ATTR_SEQUENCE = 0x0001;

const ATTR_CONTROL_AVAILABLE = 0x0001;
const ATTR_RAW_MODE_FAN_CODE = 0x0002;
const ATTR_OPERATION_CODE = 0x0003;
const ATTR_MAIN_LINK_VALID = 0x0004;
const ATTR_PANEL_LINK_VALID = 0x0005;
const ATTR_MAIN_AGE_MS = 0x0006;
const ATTR_PANEL_AGE_MS = 0x0007;
const ATTR_SENSOR_1_RAW = 0x0008;
const ATTR_SENSOR_2_RAW = 0x0009;
const ATTR_MITM_ACTIVE = 0x000A;
const ATTR_COMMAND_PENDING = 0x000B;
const ATTR_COMMAND_SEQUENCE = 0x000C;
const ATTR_CHECKSUM_ERRORS = 0x000D;
const ATTR_FRAMING_ERRORS = 0x000E;
const ATTR_INJECTED_FRAMES = 0x000F;
const ATTR_PANEL_EVENT = 0x0010;
const ATTR_COMMAND_STATUS = 0x0011;
const ATTR_COMMAND_TIMEOUTS = 0x0012;
const ATTR_QUIET = 0x0020;
const ATTR_UNITS_FAHRENHEIT = 0x0021;
const ATTR_TIMER = 0x0022;

const ZCL_BOOL = 0x10;
const ZCL_UINT8 = 0x20;
const ZCL_UINT32 = 0x23;
const ZCL_INT32 = 0x2b;
const ZCL_INT16 = 0x29;
const ZCL_ENUM8 = 0x30;

function attribute(data, id, ...names) {
    if (!data) return undefined;
    for (const name of names) {
        if (data[name] !== undefined) return data[name];
    }
    if (data[id] !== undefined) return data[id];
    if (data[String(id)] !== undefined) return data[String(id)];
    const hex = `0x${id.toString(16).padStart(4, '0')}`;
    if (data[hex] !== undefined) return data[hex];
    return data[`attr${id}`];
}

function safeNumber(value) {
    const result = Number(value);
    return Number.isFinite(result) ? result : undefined;
}

const fzThermostat = {
    cluster: THERMOSTAT_CLUSTER_NAME,
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg) => {
        const data = msg.data || {};
        const result = {};
        const systemMode = attribute(data, THERMOSTAT_ATTR_SYSTEM_MODE, 'systemMode', 'system_mode');
        const cooling = attribute(data, THERMOSTAT_ATTR_OCCUPIED_COOLING_SETPOINT,
            'occupiedCoolingSetpoint', 'occupied_cooling_setpoint');
        const local = attribute(data, THERMOSTAT_ATTR_LOCAL_TEMPERATURE, 'localTemperature', 'local_temperature');
        if (systemMode !== undefined) result.system_mode = {
            0: 'off', 3: 'cool', 7: 'fan_only', 8: 'dry',
        }[Number(systemMode)] || 'off';
        if (cooling !== undefined) result.occupied_cooling_setpoint = Number(cooling) / 100;
        if (local !== undefined && Number(local) !== -32768) result.local_temperature = Number(local) / 100;
        return result;
    },
};

const fzFanControl = {
    cluster: FAN_CONTROL_CLUSTER_NAME,
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg) => {
        const mode = attribute(msg.data || {}, FAN_ATTR_MODE, 'fanMode', 'fan_mode');
        if (mode === undefined) return {};
        const decoded = {1: 'low', 3: 'high'}[Number(mode)];
        return decoded === undefined ? {} : {fan_mode: decoded};
    },
};

const fzDiagnostics = {
    cluster: KLIMA_CLUSTER_NAME,
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg) => {
        const data = msg.data || {};
        const result = {};
        const fields = [
            ['control_available', ATTR_CONTROL_AVAILABLE, 'controlAvailable'],
            ['raw_mode_fan_code', ATTR_RAW_MODE_FAN_CODE, 'rawModeFanCode'],
            ['operation_code', ATTR_OPERATION_CODE, 'operationCode'],
            ['main_link_valid', ATTR_MAIN_LINK_VALID, 'mainLinkValid'],
            ['panel_link_valid', ATTR_PANEL_LINK_VALID, 'panelLinkValid'],
            ['main_age_ms', ATTR_MAIN_AGE_MS, 'mainAgeMs'],
            ['panel_age_ms', ATTR_PANEL_AGE_MS, 'panelAgeMs'],
            ['sensor_1_raw', ATTR_SENSOR_1_RAW, 'sensor1Raw'],
            ['sensor_2_raw', ATTR_SENSOR_2_RAW, 'sensor2Raw'],
            ['mitm_active', ATTR_MITM_ACTIVE, 'mitmActive'],
            ['command_pending', ATTR_COMMAND_PENDING, 'commandPending'],
            ['command_sequence', ATTR_COMMAND_SEQUENCE, 'commandSequence'],
            ['checksum_errors', ATTR_CHECKSUM_ERRORS, 'checksumErrors'],
            ['framing_errors', ATTR_FRAMING_ERRORS, 'framingErrors'],
            ['injected_frames', ATTR_INJECTED_FRAMES, 'injectedFrames'],
            ['panel_event', ATTR_PANEL_EVENT, 'panelEvent'],
            ['command_status', ATTR_COMMAND_STATUS, 'commandStatus'],
            ['command_timeouts', ATTR_COMMAND_TIMEOUTS, 'commandTimeouts'],
            ['quiet', ATTR_QUIET, 'quiet'],
            ['units_fahrenheit', ATTR_UNITS_FAHRENHEIT, 'unitsFahrenheit'],
            ['timer', ATTR_TIMER, 'timer'],
        ];
        for (const [name, id, alias] of fields) {
            const value = attribute(data, id, alias);
            if (value !== undefined) {
                result[name] = name === 'command_status' ?
                    ({0: 'none', 1: 'pending', 2: 'confirmed', 3: 'timed_out', 4: 'cancelled'}[Number(value)] || 'none') : value;
            }
        }
        return result;
    },
};

function requireControl(meta) {
    if (!meta || !meta.state || meta.state.control_available !== true) {
        throw new Error('Klima WiFi control is unavailable (passive profile, stale link, or unverified state)');
    }
}

const tzKlima = {
    key: ['system_mode', 'occupied_cooling_setpoint', 'fan_mode',
        'quiet', 'units_fahrenheit'],
    convertSet: async (entity, key, value, meta) => {
        requireControl(meta);
        const endpoint = meta.device.getEndpoint(1);
        if (key === 'system_mode') {
            const modes = {off: 0, cool: 3, fan_only: 7, dry: 8};
            if (!Object.prototype.hasOwnProperty.call(modes, value)) throw new Error(`Unsupported system mode: ${value}`);
            await endpoint.write(THERMOSTAT_CLUSTER, {systemMode: modes[value]});
            return {};
        }
        if (key === 'occupied_cooling_setpoint') {
            const celsius = safeNumber(value);
            if (celsius === undefined || celsius < 18 || celsius > 32) throw new Error('Setpoint must be 18-32 °C');
            await endpoint.write(THERMOSTAT_CLUSTER, {occupiedCoolingSetpoint: Math.round(celsius * 100)});
            return {};
        }
        if (key === 'fan_mode') {
            const modes = {low: 1, high: 3};
            if (!Object.prototype.hasOwnProperty.call(modes, value)) throw new Error(`Unsupported fan mode: ${value}`);
            await endpoint.write(FAN_CONTROL_CLUSTER, {fanMode: modes[value]});
            return {};
        }
        if (['quiet', 'units_fahrenheit'].includes(key)) {
            const enabled = value === true || value === 'ON' || value === 'on';
            const disabled = value === false || value === 'OFF' || value === 'off';
            if (!enabled && !disabled) throw new Error(`${key} must be ON or OFF`);
            const attrs = {quiet: ATTR_QUIET, units_fahrenheit: ATTR_UNITS_FAHRENHEIT};
            await endpoint.write(KLIMA_CLUSTER, {[attrs[key]]: enabled},
                {manufacturerCode: MANUFACTURER_CODE});
            return {};
        }
        throw new Error(`Unsupported Klima WiFi key: ${key}`);
    },
    convertGet: async (entity, key, meta) => {
        const endpoint = meta.device.getEndpoint(1);
        const requests = {
            system_mode: [THERMOSTAT_CLUSTER, [THERMOSTAT_ATTR_SYSTEM_MODE]],
            occupied_cooling_setpoint: [THERMOSTAT_CLUSTER, [THERMOSTAT_ATTR_OCCUPIED_COOLING_SETPOINT]],
            fan_mode: [FAN_CONTROL_CLUSTER, [FAN_ATTR_MODE]],
            quiet: [KLIMA_CLUSTER, [ATTR_QUIET]],
            units_fahrenheit: [KLIMA_CLUSTER, [ATTR_UNITS_FAHRENHEIT]],
        };
        const request = requests[key];
        if (request) await endpoint.read(request[0], request[1],
            request[0] === KLIMA_CLUSTER ? {manufacturerCode: MANUFACTURER_CODE} : undefined);
    },
};

const diagnosticsAttrs = [
    ATTR_CONTROL_AVAILABLE, ATTR_RAW_MODE_FAN_CODE, ATTR_OPERATION_CODE,
    ATTR_MAIN_LINK_VALID, ATTR_PANEL_LINK_VALID, ATTR_MAIN_AGE_MS, ATTR_PANEL_AGE_MS,
    ATTR_SENSOR_1_RAW, ATTR_SENSOR_2_RAW, ATTR_MITM_ACTIVE, ATTR_COMMAND_PENDING,
    ATTR_COMMAND_SEQUENCE, ATTR_CHECKSUM_ERRORS, ATTR_FRAMING_ERRORS,
    ATTR_INJECTED_FRAMES, ATTR_PANEL_EVENT, ATTR_COMMAND_STATUS, ATTR_COMMAND_TIMEOUTS,
    ATTR_QUIET, ATTR_UNITS_FAHRENHEIT, ATTR_TIMER,
];

async function readDiagnostics(endpoint) {
    for (let i = 0; i < diagnosticsAttrs.length; i += 3) {
        await endpoint.read(KLIMA_CLUSTER, diagnosticsAttrs.slice(i, i + 3), {manufacturerCode: MANUFACTURER_CODE});
    }
}

async function configureDiagnostics(endpoint, coordinatorEndpoint) {
    await reporting.bind(endpoint, coordinatorEndpoint, [THERMOSTAT_CLUSTER_NAME, FAN_CONTROL_CLUSTER_NAME, KLIMA_CLUSTER]);
    for (const attr of diagnosticsAttrs) {
        const type = [ATTR_CONTROL_AVAILABLE, ATTR_MAIN_LINK_VALID, ATTR_PANEL_LINK_VALID,
            ATTR_MITM_ACTIVE, ATTR_COMMAND_PENDING, ATTR_PANEL_EVENT].includes(attr) ? ZCL_BOOL :
            [ATTR_MAIN_AGE_MS, ATTR_PANEL_AGE_MS].includes(attr) ? ZCL_INT32 :
            [ATTR_COMMAND_SEQUENCE, ATTR_CHECKSUM_ERRORS, ATTR_FRAMING_ERRORS, ATTR_INJECTED_FRAMES,
                ATTR_COMMAND_TIMEOUTS].includes(attr) ? ZCL_UINT32 :
            [ATTR_QUIET, ATTR_UNITS_FAHRENHEIT, ATTR_TIMER].includes(attr) ? ZCL_BOOL : ZCL_UINT8;
        const record = {
            attribute: {ID: attr, type}, minimumReportInterval: 1, maximumReportInterval: 30,
        };
        if (type !== ZCL_BOOL) record.reportableChange = 1;
        await endpoint.configureReporting(KLIMA_CLUSTER, [record], {manufacturerCode: MANUFACTURER_CODE});
    }
}

async function configureStandardReporting(endpoint) {
    const thermostat = [
        {attribute: {ID: THERMOSTAT_ATTR_LOCAL_TEMPERATURE, type: ZCL_INT16}, minimumReportInterval: 5, maximumReportInterval: 60, reportableChange: 10},
        {attribute: {ID: THERMOSTAT_ATTR_OCCUPIED_COOLING_SETPOINT, type: ZCL_INT16}, minimumReportInterval: 1, maximumReportInterval: 30, reportableChange: 100},
        {attribute: {ID: THERMOSTAT_ATTR_SYSTEM_MODE, type: ZCL_ENUM8}, minimumReportInterval: 1, maximumReportInterval: 30},
    ];
    const fan = [
        {attribute: {ID: FAN_ATTR_MODE, type: ZCL_ENUM8}, minimumReportInterval: 1, maximumReportInterval: 30},
        {attribute: {ID: FAN_ATTR_SEQUENCE, type: ZCL_ENUM8}, minimumReportInterval: 1, maximumReportInterval: 60},
    ];
    await endpoint.configureReporting(THERMOSTAT_CLUSTER, thermostat);
    await endpoint.configureReporting(FAN_CONTROL_CLUSTER, fan);
}

async function safeConfigure(label, fn) {
    try {
        await fn();
    } catch (error) {
        // A coordinator may reject one optional report record; keep the
        // remaining diagnostic reads and reports active.
        console.warn(`Klima WiFi converter: ${label} failed: ${error.message}`);
    }
}

function diagnosticNumeric(name, unit, min, max) {
    return exposes.numeric(name, ea.STATE).withUnit(unit).withValueMin(min).withValueMax(max);
}

module.exports = [{
    zigbeeModel: ['Klima WiFi AC Bridge'],
    model: 'klima-wifi-ac-bridge',
    vendor: 'Klima WiFi contributors',
    description: 'ESP32-C6 native always-powered Zigbee End Device AC bridge',
    fromZigbee: [fzThermostat, fzFanControl, fzDiagnostics],
    toZigbee: [tzKlima],
    exposes: [
        e.climate()
            .withSetpoint('occupied_cooling_setpoint', 18, 32, 1)
            .withSystemMode(['off', 'cool', 'fan_only', 'dry'])
            .withFanMode(['low', 'high']),
        exposes.binary('quiet', ea.ALL, true, false),
        exposes.binary('units_fahrenheit', ea.ALL, true, false),
        exposes.binary('timer', ea.STATE, true, false),
        diagnosticNumeric('raw_mode_fan_code', '', 0, 255),
        diagnosticNumeric('operation_code', '', 0, 255),
        exposes.binary('control_available', ea.STATE, true, false),
        exposes.binary('main_link_valid', ea.STATE, true, false),
        exposes.binary('panel_link_valid', ea.STATE, true, false),
        diagnosticNumeric('main_age_ms', 'ms', -1, 2147483647),
        diagnosticNumeric('panel_age_ms', 'ms', -1, 2147483647),
        diagnosticNumeric('sensor_1_raw', '', 0, 255),
        diagnosticNumeric('sensor_2_raw', '', 0, 255),
        exposes.binary('mitm_active', ea.STATE, true, false),
        exposes.binary('command_pending', ea.STATE, true, false),
        diagnosticNumeric('command_sequence', '', 0, 4294967295),
        exposes.enum('command_status', ea.STATE, ['none', 'pending', 'confirmed', 'timed_out', 'cancelled']),
        diagnosticNumeric('command_timeouts', '', 0, 4294967295),
        diagnosticNumeric('checksum_errors', '', 0, 4294967295),
        diagnosticNumeric('framing_errors', '', 0, 4294967295),
        diagnosticNumeric('injected_frames', '', 0, 4294967295),
        exposes.binary('panel_event', ea.STATE, true, false),
    ],
    endpoint: () => ({default: 1}),
    meta: {multiEndpoint: false},
    configure: async (device, coordinatorEndpoint) => {
        const endpoint = device.getEndpoint(1);
        await safeConfigure('standard reporting', () => configureStandardReporting(endpoint));
        await safeConfigure('diagnostic reporting', () => configureDiagnostics(endpoint, coordinatorEndpoint));
        await safeConfigure('thermostat read', () => endpoint.read(THERMOSTAT_CLUSTER, [THERMOSTAT_ATTR_LOCAL_TEMPERATURE,
            THERMOSTAT_ATTR_OCCUPIED_COOLING_SETPOINT, THERMOSTAT_ATTR_SYSTEM_MODE]));
        await safeConfigure('fan read', () => endpoint.read(FAN_CONTROL_CLUSTER, [FAN_ATTR_MODE, FAN_ATTR_SEQUENCE]));
        await safeConfigure('diagnostic read', () => readDiagnostics(endpoint));
    },
}];
