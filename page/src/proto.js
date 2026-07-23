/*eslint-disable block-scoped-var, id-length, no-control-regex, no-magic-numbers, no-prototype-builtins, no-redeclare, no-shadow, no-var, sort-vars*/
import * as $protobuf from "protobufjs/minimal";

// Common aliases
const $Reader = $protobuf.Reader, $Writer = $protobuf.Writer, $util = $protobuf.util;

// Exported root namespace
const $root = $protobuf.roots["default"] || ($protobuf.roots["default"] = {});

export const SensorChannelData = $root.SensorChannelData = (() => {

    /**
     * Properties of a SensorChannelData.
     * @exports ISensorChannelData
     * @interface ISensorChannelData
     * @property {number|null} [voltage] SensorChannelData voltage
     * @property {number|null} [current] SensorChannelData current
     * @property {number|null} [power] SensorChannelData power
     */

    /**
     * Constructs a new SensorChannelData.
     * @exports SensorChannelData
     * @classdesc Represents a SensorChannelData.
     * @implements ISensorChannelData
     * @constructor
     * @param {ISensorChannelData=} [properties] Properties to set
     */
    function SensorChannelData(properties) {
        if (properties)
            for (let keys = Object.keys(properties), i = 0; i < keys.length; ++i)
                if (properties[keys[i]] != null)
                    this[keys[i]] = properties[keys[i]];
    }

    /**
     * SensorChannelData voltage.
     * @member {number} voltage
     * @memberof SensorChannelData
     * @instance
     */
    SensorChannelData.prototype.voltage = 0;

    /**
     * SensorChannelData current.
     * @member {number} current
     * @memberof SensorChannelData
     * @instance
     */
    SensorChannelData.prototype.current = 0;

    /**
     * SensorChannelData power.
     * @member {number} power
     * @memberof SensorChannelData
     * @instance
     */
    SensorChannelData.prototype.power = 0;

    /**
     * Creates a new SensorChannelData instance using the specified properties.
     * @function create
     * @memberof SensorChannelData
     * @static
     * @param {ISensorChannelData=} [properties] Properties to set
     * @returns {SensorChannelData} SensorChannelData instance
     */
    SensorChannelData.create = function create(properties) {
        return new SensorChannelData(properties);
    };

    /**
     * Encodes the specified SensorChannelData message. Does not implicitly {@link SensorChannelData.verify|verify} messages.
     * @function encode
     * @memberof SensorChannelData
     * @static
     * @param {ISensorChannelData} message SensorChannelData message or plain object to encode
     * @param {$protobuf.Writer} [writer] Writer to encode to
     * @returns {$protobuf.Writer} Writer
     */
    SensorChannelData.encode = function encode(message, writer) {
        if (!writer)
            writer = $Writer.create();
        if (message.voltage != null && Object.hasOwnProperty.call(message, "voltage"))
            writer.uint32(/* id 1, wireType 5 =*/13).float(message.voltage);
        if (message.current != null && Object.hasOwnProperty.call(message, "current"))
            writer.uint32(/* id 2, wireType 5 =*/21).float(message.current);
        if (message.power != null && Object.hasOwnProperty.call(message, "power"))
            writer.uint32(/* id 3, wireType 5 =*/29).float(message.power);
        return writer;
    };

    /**
     * Encodes the specified SensorChannelData message, length delimited. Does not implicitly {@link SensorChannelData.verify|verify} messages.
     * @function encodeDelimited
     * @memberof SensorChannelData
     * @static
     * @param {ISensorChannelData} message SensorChannelData message or plain object to encode
     * @param {$protobuf.Writer} [writer] Writer to encode to
     * @returns {$protobuf.Writer} Writer
     */
    SensorChannelData.encodeDelimited = function encodeDelimited(message, writer) {
        return this.encode(message, writer).ldelim();
    };

    /**
     * Decodes a SensorChannelData message from the specified reader or buffer.
     * @function decode
     * @memberof SensorChannelData
     * @static
     * @param {$protobuf.Reader|Uint8Array} reader Reader or buffer to decode from
     * @param {number} [length] Message length if known beforehand
     * @returns {SensorChannelData} SensorChannelData
     * @throws {Error} If the payload is not a reader or valid buffer
     * @throws {$protobuf.util.ProtocolError} If required fields are missing
     */
    SensorChannelData.decode = function decode(reader, length, error) {
        if (!(reader instanceof $Reader))
            reader = $Reader.create(reader);
        let end = length === undefined ? reader.len : reader.pos + length, message = new $root.SensorChannelData();
        while (reader.pos < end) {
            let tag = reader.uint32();
            if (tag === error)
                break;
            switch (tag >>> 3) {
            case 1: {
                    message.voltage = reader.float();
                    break;
                }
            case 2: {
                    message.current = reader.float();
                    break;
                }
            case 3: {
                    message.power = reader.float();
                    break;
                }
            default:
                reader.skipType(tag & 7);
                break;
            }
        }
        return message;
    };

    /**
     * Decodes a SensorChannelData message from the specified reader or buffer, length delimited.
     * @function decodeDelimited
     * @memberof SensorChannelData
     * @static
     * @param {$protobuf.Reader|Uint8Array} reader Reader or buffer to decode from
     * @returns {SensorChannelData} SensorChannelData
     * @throws {Error} If the payload is not a reader or valid buffer
     * @throws {$protobuf.util.ProtocolError} If required fields are missing
     */
    SensorChannelData.decodeDelimited = function decodeDelimited(reader) {
        if (!(reader instanceof $Reader))
            reader = new $Reader(reader);
        return this.decode(reader, reader.uint32());
    };

    /**
     * Verifies a SensorChannelData message.
     * @function verify
     * @memberof SensorChannelData
     * @static
     * @param {Object.<string,*>} message Plain object to verify
     * @returns {string|null} `null` if valid, otherwise the reason why it is not
     */
    SensorChannelData.verify = function verify(message) {
        if (typeof message !== "object" || message === null)
            return "object expected";
        if (message.voltage != null && message.hasOwnProperty("voltage"))
            if (typeof message.voltage !== "number")
                return "voltage: number expected";
        if (message.current != null && message.hasOwnProperty("current"))
            if (typeof message.current !== "number")
                return "current: number expected";
        if (message.power != null && message.hasOwnProperty("power"))
            if (typeof message.power !== "number")
                return "power: number expected";
        return null;
    };

    /**
     * Creates a SensorChannelData message from a plain object. Also converts values to their respective internal types.
     * @function fromObject
     * @memberof SensorChannelData
     * @static
     * @param {Object.<string,*>} object Plain object
     * @returns {SensorChannelData} SensorChannelData
     */
    SensorChannelData.fromObject = function fromObject(object) {
        if (object instanceof $root.SensorChannelData)
            return object;
        let message = new $root.SensorChannelData();
        if (object.voltage != null)
            message.voltage = Number(object.voltage);
        if (object.current != null)
            message.current = Number(object.current);
        if (object.power != null)
            message.power = Number(object.power);
        return message;
    };

    /**
     * Creates a plain object from a SensorChannelData message. Also converts values to other types if specified.
     * @function toObject
     * @memberof SensorChannelData
     * @static
     * @param {SensorChannelData} message SensorChannelData
     * @param {$protobuf.IConversionOptions} [options] Conversion options
     * @returns {Object.<string,*>} Plain object
     */
    SensorChannelData.toObject = function toObject(message, options) {
        if (!options)
            options = {};
        let object = {};
        if (options.defaults) {
            object.voltage = 0;
            object.current = 0;
            object.power = 0;
        }
        if (message.voltage != null && message.hasOwnProperty("voltage"))
            object.voltage = options.json && !isFinite(message.voltage) ? String(message.voltage) : message.voltage;
        if (message.current != null && message.hasOwnProperty("current"))
            object.current = options.json && !isFinite(message.current) ? String(message.current) : message.current;
        if (message.power != null && message.hasOwnProperty("power"))
            object.power = options.json && !isFinite(message.power) ? String(message.power) : message.power;
        return object;
    };

    /**
     * Converts this SensorChannelData to JSON.
     * @function toJSON
     * @memberof SensorChannelData
     * @instance
     * @returns {Object.<string,*>} JSON object
     */
    SensorChannelData.prototype.toJSON = function toJSON() {
        return this.constructor.toObject(this, $protobuf.util.toJSONOptions);
    };

    /**
     * Gets the default type url for SensorChannelData
     * @function getTypeUrl
     * @memberof SensorChannelData
     * @static
     * @param {string} [typeUrlPrefix] your custom typeUrlPrefix(default "type.googleapis.com")
     * @returns {string} The default type url
     */
    SensorChannelData.getTypeUrl = function getTypeUrl(typeUrlPrefix) {
        if (typeUrlPrefix === undefined) {
            typeUrlPrefix = "type.googleapis.com";
        }
        return typeUrlPrefix + "/SensorChannelData";
    };

    return SensorChannelData;
})();

export const SensorData = $root.SensorData = (() => {

    /**
     * Properties of a SensorData.
     * @exports ISensorData
     * @interface ISensorData
     * @property {ISensorChannelData|null} [usb] SensorData usb
     * @property {ISensorChannelData|null} [main] SensorData main
     * @property {ISensorChannelData|null} [vin] SensorData vin
     * @property {number|Long|null} [timestampMs] SensorData timestampMs
     * @property {number|Long|null} [uptimeMs] SensorData uptimeMs
     */

    /**
     * Constructs a new SensorData.
     * @exports SensorData
     * @classdesc Represents a SensorData.
     * @implements ISensorData
     * @constructor
     * @param {ISensorData=} [properties] Properties to set
     */
    function SensorData(properties) {
        if (properties)
            for (let keys = Object.keys(properties), i = 0; i < keys.length; ++i)
                if (properties[keys[i]] != null)
                    this[keys[i]] = properties[keys[i]];
    }

    /**
     * SensorData usb.
     * @member {ISensorChannelData|null|undefined} usb
     * @memberof SensorData
     * @instance
     */
    SensorData.prototype.usb = null;

    /**
     * SensorData main.
     * @member {ISensorChannelData|null|undefined} main
     * @memberof SensorData
     * @instance
     */
    SensorData.prototype.main = null;

    /**
     * SensorData vin.
     * @member {ISensorChannelData|null|undefined} vin
     * @memberof SensorData
     * @instance
     */
    SensorData.prototype.vin = null;

    /**
     * SensorData timestampMs.
     * @member {number|Long} timestampMs
     * @memberof SensorData
     * @instance
     */
    SensorData.prototype.timestampMs = $util.Long ? $util.Long.fromBits(0,0,true) : 0;

    /**
     * SensorData uptimeMs.
     * @member {number|Long} uptimeMs
     * @memberof SensorData
     * @instance
     */
    SensorData.prototype.uptimeMs = $util.Long ? $util.Long.fromBits(0,0,true) : 0;

    /**
     * Creates a new SensorData instance using the specified properties.
     * @function create
     * @memberof SensorData
     * @static
     * @param {ISensorData=} [properties] Properties to set
     * @returns {SensorData} SensorData instance
     */
    SensorData.create = function create(properties) {
        return new SensorData(properties);
    };

    /**
     * Encodes the specified SensorData message. Does not implicitly {@link SensorData.verify|verify} messages.
     * @function encode
     * @memberof SensorData
     * @static
     * @param {ISensorData} message SensorData message or plain object to encode
     * @param {$protobuf.Writer} [writer] Writer to encode to
     * @returns {$protobuf.Writer} Writer
     */
    SensorData.encode = function encode(message, writer) {
        if (!writer)
            writer = $Writer.create();
        if (message.usb != null && Object.hasOwnProperty.call(message, "usb"))
            $root.SensorChannelData.encode(message.usb, writer.uint32(/* id 1, wireType 2 =*/10).fork()).ldelim();
        if (message.main != null && Object.hasOwnProperty.call(message, "main"))
            $root.SensorChannelData.encode(message.main, writer.uint32(/* id 2, wireType 2 =*/18).fork()).ldelim();
        if (message.vin != null && Object.hasOwnProperty.call(message, "vin"))
            $root.SensorChannelData.encode(message.vin, writer.uint32(/* id 3, wireType 2 =*/26).fork()).ldelim();
        if (message.timestampMs != null && Object.hasOwnProperty.call(message, "timestampMs"))
            writer.uint32(/* id 4, wireType 0 =*/32).uint64(message.timestampMs);
        if (message.uptimeMs != null && Object.hasOwnProperty.call(message, "uptimeMs"))
            writer.uint32(/* id 5, wireType 0 =*/40).uint64(message.uptimeMs);
        return writer;
    };

    /**
     * Encodes the specified SensorData message, length delimited. Does not implicitly {@link SensorData.verify|verify} messages.
     * @function encodeDelimited
     * @memberof SensorData
     * @static
     * @param {ISensorData} message SensorData message or plain object to encode
     * @param {$protobuf.Writer} [writer] Writer to encode to
     * @returns {$protobuf.Writer} Writer
     */
    SensorData.encodeDelimited = function encodeDelimited(message, writer) {
        return this.encode(message, writer).ldelim();
    };

    /**
     * Decodes a SensorData message from the specified reader or buffer.
     * @function decode
     * @memberof SensorData
     * @static
     * @param {$protobuf.Reader|Uint8Array} reader Reader or buffer to decode from
     * @param {number} [length] Message length if known beforehand
     * @returns {SensorData} SensorData
     * @throws {Error} If the payload is not a reader or valid buffer
     * @throws {$protobuf.util.ProtocolError} If required fields are missing
     */
    SensorData.decode = function decode(reader, length, error) {
        if (!(reader instanceof $Reader))
            reader = $Reader.create(reader);
        let end = length === undefined ? reader.len : reader.pos + length, message = new $root.SensorData();
        while (reader.pos < end) {
            let tag = reader.uint32();
            if (tag === error)
                break;
            switch (tag >>> 3) {
            case 1: {
                    message.usb = $root.SensorChannelData.decode(reader, reader.uint32());
                    break;
                }
            case 2: {
                    message.main = $root.SensorChannelData.decode(reader, reader.uint32());
                    break;
                }
            case 3: {
                    message.vin = $root.SensorChannelData.decode(reader, reader.uint32());
                    break;
                }
            case 4: {
                    message.timestampMs = reader.uint64();
                    break;
                }
            case 5: {
                    message.uptimeMs = reader.uint64();
                    break;
                }
            default:
                reader.skipType(tag & 7);
                break;
            }
        }
        return message;
    };

    /**
     * Decodes a SensorData message from the specified reader or buffer, length delimited.
     * @function decodeDelimited
     * @memberof SensorData
     * @static
     * @param {$protobuf.Reader|Uint8Array} reader Reader or buffer to decode from
     * @returns {SensorData} SensorData
     * @throws {Error} If the payload is not a reader or valid buffer
     * @throws {$protobuf.util.ProtocolError} If required fields are missing
     */
    SensorData.decodeDelimited = function decodeDelimited(reader) {
        if (!(reader instanceof $Reader))
            reader = new $Reader(reader);
        return this.decode(reader, reader.uint32());
    };

    /**
     * Verifies a SensorData message.
     * @function verify
     * @memberof SensorData
     * @static
     * @param {Object.<string,*>} message Plain object to verify
     * @returns {string|null} `null` if valid, otherwise the reason why it is not
     */
    SensorData.verify = function verify(message) {
        if (typeof message !== "object" || message === null)
            return "object expected";
        if (message.usb != null && message.hasOwnProperty("usb")) {
            let error = $root.SensorChannelData.verify(message.usb);
            if (error)
                return "usb." + error;
        }
        if (message.main != null && message.hasOwnProperty("main")) {
            let error = $root.SensorChannelData.verify(message.main);
            if (error)
                return "main." + error;
        }
        if (message.vin != null && message.hasOwnProperty("vin")) {
            let error = $root.SensorChannelData.verify(message.vin);
            if (error)
                return "vin." + error;
        }
        if (message.timestampMs != null && message.hasOwnProperty("timestampMs"))
            if (!$util.isInteger(message.timestampMs) && !(message.timestampMs && $util.isInteger(message.timestampMs.low) && $util.isInteger(message.timestampMs.high)))
                return "timestampMs: integer|Long expected";
        if (message.uptimeMs != null && message.hasOwnProperty("uptimeMs"))
            if (!$util.isInteger(message.uptimeMs) && !(message.uptimeMs && $util.isInteger(message.uptimeMs.low) && $util.isInteger(message.uptimeMs.high)))
                return "uptimeMs: integer|Long expected";
        return null;
    };

    /**
     * Creates a SensorData message from a plain object. Also converts values to their respective internal types.
     * @function fromObject
     * @memberof SensorData
     * @static
     * @param {Object.<string,*>} object Plain object
     * @returns {SensorData} SensorData
     */
    SensorData.fromObject = function fromObject(object) {
        if (object instanceof $root.SensorData)
            return object;
        let message = new $root.SensorData();
        if (object.usb != null) {
            if (typeof object.usb !== "object")
                throw TypeError(".SensorData.usb: object expected");
            message.usb = $root.SensorChannelData.fromObject(object.usb);
        }
        if (object.main != null) {
            if (typeof object.main !== "object")
                throw TypeError(".SensorData.main: object expected");
            message.main = $root.SensorChannelData.fromObject(object.main);
        }
        if (object.vin != null) {
            if (typeof object.vin !== "object")
                throw TypeError(".SensorData.vin: object expected");
            message.vin = $root.SensorChannelData.fromObject(object.vin);
        }
        if (object.timestampMs != null)
            if ($util.Long)
                (message.timestampMs = $util.Long.fromValue(object.timestampMs)).unsigned = true;
            else if (typeof object.timestampMs === "string")
                message.timestampMs = parseInt(object.timestampMs, 10);
            else if (typeof object.timestampMs === "number")
                message.timestampMs = object.timestampMs;
            else if (typeof object.timestampMs === "object")
                message.timestampMs = new $util.LongBits(object.timestampMs.low >>> 0, object.timestampMs.high >>> 0).toNumber(true);
        if (object.uptimeMs != null)
            if ($util.Long)
                (message.uptimeMs = $util.Long.fromValue(object.uptimeMs)).unsigned = true;
            else if (typeof object.uptimeMs === "string")
                message.uptimeMs = parseInt(object.uptimeMs, 10);
            else if (typeof object.uptimeMs === "number")
                message.uptimeMs = object.uptimeMs;
            else if (typeof object.uptimeMs === "object")
                message.uptimeMs = new $util.LongBits(object.uptimeMs.low >>> 0, object.uptimeMs.high >>> 0).toNumber(true);
        return message;
    };

    /**
     * Creates a plain object from a SensorData message. Also converts values to other types if specified.
     * @function toObject
     * @memberof SensorData
     * @static
     * @param {SensorData} message SensorData
     * @param {$protobuf.IConversionOptions} [options] Conversion options
     * @returns {Object.<string,*>} Plain object
     */
    SensorData.toObject = function toObject(message, options) {
        if (!options)
            options = {};
        let object = {};
        if (options.defaults) {
            object.usb = null;
            object.main = null;
            object.vin = null;
            if ($util.Long) {
                let long = new $util.Long(0, 0, true);
                object.timestampMs = options.longs === String ? long.toString() : options.longs === Number ? long.toNumber() : long;
            } else
                object.timestampMs = options.longs === String ? "0" : 0;
            if ($util.Long) {
                let long = new $util.Long(0, 0, true);
                object.uptimeMs = options.longs === String ? long.toString() : options.longs === Number ? long.toNumber() : long;
            } else
                object.uptimeMs = options.longs === String ? "0" : 0;
        }
        if (message.usb != null && message.hasOwnProperty("usb"))
            object.usb = $root.SensorChannelData.toObject(message.usb, options);
        if (message.main != null && message.hasOwnProperty("main"))
            object.main = $root.SensorChannelData.toObject(message.main, options);
        if (message.vin != null && message.hasOwnProperty("vin"))
            object.vin = $root.SensorChannelData.toObject(message.vin, options);
        if (message.timestampMs != null && message.hasOwnProperty("timestampMs"))
            if (typeof message.timestampMs === "number")
                object.timestampMs = options.longs === String ? String(message.timestampMs) : message.timestampMs;
            else
                object.timestampMs = options.longs === String ? $util.Long.prototype.toString.call(message.timestampMs) : options.longs === Number ? new $util.LongBits(message.timestampMs.low >>> 0, message.timestampMs.high >>> 0).toNumber(true) : message.timestampMs;
        if (message.uptimeMs != null && message.hasOwnProperty("uptimeMs"))
            if (typeof message.uptimeMs === "number")
                object.uptimeMs = options.longs === String ? String(message.uptimeMs) : message.uptimeMs;
            else
                object.uptimeMs = options.longs === String ? $util.Long.prototype.toString.call(message.uptimeMs) : options.longs === Number ? new $util.LongBits(message.uptimeMs.low >>> 0, message.uptimeMs.high >>> 0).toNumber(true) : message.uptimeMs;
        return object;
    };

    /**
     * Converts this SensorData to JSON.
     * @function toJSON
     * @memberof SensorData
     * @instance
     * @returns {Object.<string,*>} JSON object
     */
    SensorData.prototype.toJSON = function toJSON() {
        return this.constructor.toObject(this, $protobuf.util.toJSONOptions);
    };

    /**
     * Gets the default type url for SensorData
     * @function getTypeUrl
     * @memberof SensorData
     * @static
     * @param {string} [typeUrlPrefix] your custom typeUrlPrefix(default "type.googleapis.com")
     * @returns {string} The default type url
     */
    SensorData.getTypeUrl = function getTypeUrl(typeUrlPrefix) {
        if (typeUrlPrefix === undefined) {
            typeUrlPrefix = "type.googleapis.com";
        }
        return typeUrlPrefix + "/SensorData";
    };

    return SensorData;
})();

export const WifiStatus = $root.WifiStatus = (() => {

    /**
     * Properties of a WifiStatus.
     * @exports IWifiStatus
     * @interface IWifiStatus
     * @property {boolean|null} [connected] WifiStatus connected
     * @property {string|null} [ssid] WifiStatus ssid
     * @property {number|null} [rssi] WifiStatus rssi
     * @property {string|null} [ipAddress] WifiStatus ipAddress
     */

    /**
     * Constructs a new WifiStatus.
     * @exports WifiStatus
     * @classdesc Represents a WifiStatus.
     * @implements IWifiStatus
     * @constructor
     * @param {IWifiStatus=} [properties] Properties to set
     */
    function WifiStatus(properties) {
        if (properties)
            for (let keys = Object.keys(properties), i = 0; i < keys.length; ++i)
                if (properties[keys[i]] != null)
                    this[keys[i]] = properties[keys[i]];
    }

    /**
     * WifiStatus connected.
     * @member {boolean} connected
     * @memberof WifiStatus
     * @instance
     */
    WifiStatus.prototype.connected = false;

    /**
     * WifiStatus ssid.
     * @member {string} ssid
     * @memberof WifiStatus
     * @instance
     */
    WifiStatus.prototype.ssid = "";

    /**
     * WifiStatus rssi.
     * @member {number} rssi
     * @memberof WifiStatus
     * @instance
     */
    WifiStatus.prototype.rssi = 0;

    /**
     * WifiStatus ipAddress.
     * @member {string} ipAddress
     * @memberof WifiStatus
     * @instance
     */
    WifiStatus.prototype.ipAddress = "";

    /**
     * Creates a new WifiStatus instance using the specified properties.
     * @function create
     * @memberof WifiStatus
     * @static
     * @param {IWifiStatus=} [properties] Properties to set
     * @returns {WifiStatus} WifiStatus instance
     */
    WifiStatus.create = function create(properties) {
        return new WifiStatus(properties);
    };

    /**
     * Encodes the specified WifiStatus message. Does not implicitly {@link WifiStatus.verify|verify} messages.
     * @function encode
     * @memberof WifiStatus
     * @static
     * @param {IWifiStatus} message WifiStatus message or plain object to encode
     * @param {$protobuf.Writer} [writer] Writer to encode to
     * @returns {$protobuf.Writer} Writer
     */
    WifiStatus.encode = function encode(message, writer) {
        if (!writer)
            writer = $Writer.create();
        if (message.connected != null && Object.hasOwnProperty.call(message, "connected"))
            writer.uint32(/* id 1, wireType 0 =*/8).bool(message.connected);
        if (message.ssid != null && Object.hasOwnProperty.call(message, "ssid"))
            writer.uint32(/* id 2, wireType 2 =*/18).string(message.ssid);
        if (message.rssi != null && Object.hasOwnProperty.call(message, "rssi"))
            writer.uint32(/* id 3, wireType 0 =*/24).int32(message.rssi);
        if (message.ipAddress != null && Object.hasOwnProperty.call(message, "ipAddress"))
            writer.uint32(/* id 4, wireType 2 =*/34).string(message.ipAddress);
        return writer;
    };

    /**
     * Encodes the specified WifiStatus message, length delimited. Does not implicitly {@link WifiStatus.verify|verify} messages.
     * @function encodeDelimited
     * @memberof WifiStatus
     * @static
     * @param {IWifiStatus} message WifiStatus message or plain object to encode
     * @param {$protobuf.Writer} [writer] Writer to encode to
     * @returns {$protobuf.Writer} Writer
     */
    WifiStatus.encodeDelimited = function encodeDelimited(message, writer) {
        return this.encode(message, writer).ldelim();
    };

    /**
     * Decodes a WifiStatus message from the specified reader or buffer.
     * @function decode
     * @memberof WifiStatus
     * @static
     * @param {$protobuf.Reader|Uint8Array} reader Reader or buffer to decode from
     * @param {number} [length] Message length if known beforehand
     * @returns {WifiStatus} WifiStatus
     * @throws {Error} If the payload is not a reader or valid buffer
     * @throws {$protobuf.util.ProtocolError} If required fields are missing
     */
    WifiStatus.decode = function decode(reader, length, error) {
        if (!(reader instanceof $Reader))
            reader = $Reader.create(reader);
        let end = length === undefined ? reader.len : reader.pos + length, message = new $root.WifiStatus();
        while (reader.pos < end) {
            let tag = reader.uint32();
            if (tag === error)
                break;
            switch (tag >>> 3) {
            case 1: {
                    message.connected = reader.bool();
                    break;
                }
            case 2: {
                    message.ssid = reader.string();
                    break;
                }
            case 3: {
                    message.rssi = reader.int32();
                    break;
                }
            case 4: {
                    message.ipAddress = reader.string();
                    break;
                }
            default:
                reader.skipType(tag & 7);
                break;
            }
        }
        return message;
    };

    /**
     * Decodes a WifiStatus message from the specified reader or buffer, length delimited.
     * @function decodeDelimited
     * @memberof WifiStatus
     * @static
     * @param {$protobuf.Reader|Uint8Array} reader Reader or buffer to decode from
     * @returns {WifiStatus} WifiStatus
     * @throws {Error} If the payload is not a reader or valid buffer
     * @throws {$protobuf.util.ProtocolError} If required fields are missing
     */
    WifiStatus.decodeDelimited = function decodeDelimited(reader) {
        if (!(reader instanceof $Reader))
            reader = new $Reader(reader);
        return this.decode(reader, reader.uint32());
    };

    /**
     * Verifies a WifiStatus message.
     * @function verify
     * @memberof WifiStatus
     * @static
     * @param {Object.<string,*>} message Plain object to verify
     * @returns {string|null} `null` if valid, otherwise the reason why it is not
     */
    WifiStatus.verify = function verify(message) {
        if (typeof message !== "object" || message === null)
            return "object expected";
        if (message.connected != null && message.hasOwnProperty("connected"))
            if (typeof message.connected !== "boolean")
                return "connected: boolean expected";
        if (message.ssid != null && message.hasOwnProperty("ssid"))
            if (!$util.isString(message.ssid))
                return "ssid: string expected";
        if (message.rssi != null && message.hasOwnProperty("rssi"))
            if (!$util.isInteger(message.rssi))
                return "rssi: integer expected";
        if (message.ipAddress != null && message.hasOwnProperty("ipAddress"))
            if (!$util.isString(message.ipAddress))
                return "ipAddress: string expected";
        return null;
    };

    /**
     * Creates a WifiStatus message from a plain object. Also converts values to their respective internal types.
     * @function fromObject
     * @memberof WifiStatus
     * @static
     * @param {Object.<string,*>} object Plain object
     * @returns {WifiStatus} WifiStatus
     */
    WifiStatus.fromObject = function fromObject(object) {
        if (object instanceof $root.WifiStatus)
            return object;
        let message = new $root.WifiStatus();
        if (object.connected != null)
            message.connected = Boolean(object.connected);
        if (object.ssid != null)
            message.ssid = String(object.ssid);
        if (object.rssi != null)
            message.rssi = object.rssi | 0;
        if (object.ipAddress != null)
            message.ipAddress = String(object.ipAddress);
        return message;
    };

    /**
     * Creates a plain object from a WifiStatus message. Also converts values to other types if specified.
     * @function toObject
     * @memberof WifiStatus
     * @static
     * @param {WifiStatus} message WifiStatus
     * @param {$protobuf.IConversionOptions} [options] Conversion options
     * @returns {Object.<string,*>} Plain object
     */
    WifiStatus.toObject = function toObject(message, options) {
        if (!options)
            options = {};
        let object = {};
        if (options.defaults) {
            object.connected = false;
            object.ssid = "";
            object.rssi = 0;
            object.ipAddress = "";
        }
        if (message.connected != null && message.hasOwnProperty("connected"))
            object.connected = message.connected;
        if (message.ssid != null && message.hasOwnProperty("ssid"))
            object.ssid = message.ssid;
        if (message.rssi != null && message.hasOwnProperty("rssi"))
            object.rssi = message.rssi;
        if (message.ipAddress != null && message.hasOwnProperty("ipAddress"))
            object.ipAddress = message.ipAddress;
        return object;
    };

    /**
     * Converts this WifiStatus to JSON.
     * @function toJSON
     * @memberof WifiStatus
     * @instance
     * @returns {Object.<string,*>} JSON object
     */
    WifiStatus.prototype.toJSON = function toJSON() {
        return this.constructor.toObject(this, $protobuf.util.toJSONOptions);
    };

    /**
     * Gets the default type url for WifiStatus
     * @function getTypeUrl
     * @memberof WifiStatus
     * @static
     * @param {string} [typeUrlPrefix] your custom typeUrlPrefix(default "type.googleapis.com")
     * @returns {string} The default type url
     */
    WifiStatus.getTypeUrl = function getTypeUrl(typeUrlPrefix) {
        if (typeUrlPrefix === undefined) {
            typeUrlPrefix = "type.googleapis.com";
        }
        return typeUrlPrefix + "/WifiStatus";
    };

    return WifiStatus;
})();

export const EventData = $root.EventData = (() => {

    /**
     * Properties of an EventData.
     * @exports IEventData
     * @interface IEventData
     * @property {number|null} [level] EventData level
     * @property {number|Long|null} [timestampMs] EventData timestampMs
     * @property {number|Long|null} [uptimeMs] EventData uptimeMs
     * @property {string|null} [message] EventData message
     */

    /**
     * Constructs a new EventData.
     * @exports EventData
     * @classdesc Represents an EventData.
     * @implements IEventData
     * @constructor
     * @param {IEventData=} [properties] Properties to set
     */
    function EventData(properties) {
        if (properties)
            for (let keys = Object.keys(properties), i = 0; i < keys.length; ++i)
                if (properties[keys[i]] != null)
                    this[keys[i]] = properties[keys[i]];
    }

    /**
     * EventData level.
     * @member {number} level
     * @memberof EventData
     * @instance
     */
    EventData.prototype.level = 0;

    /**
     * EventData timestampMs.
     * @member {number|Long} timestampMs
     * @memberof EventData
     * @instance
     */
    EventData.prototype.timestampMs = $util.Long ? $util.Long.fromBits(0,0,true) : 0;

    /**
     * EventData uptimeMs.
     * @member {number|Long} uptimeMs
     * @memberof EventData
     * @instance
     */
    EventData.prototype.uptimeMs = $util.Long ? $util.Long.fromBits(0,0,true) : 0;

    /**
     * EventData message.
     * @member {string} message
     * @memberof EventData
     * @instance
     */
    EventData.prototype.message = "";

    /**
     * Creates a new EventData instance using the specified properties.
     * @function create
     * @memberof EventData
     * @static
     * @param {IEventData=} [properties] Properties to set
     * @returns {EventData} EventData instance
     */
    EventData.create = function create(properties) {
        return new EventData(properties);
    };

    /**
     * Encodes the specified EventData message. Does not implicitly {@link EventData.verify|verify} messages.
     * @function encode
     * @memberof EventData
     * @static
     * @param {IEventData} message EventData message or plain object to encode
     * @param {$protobuf.Writer} [writer] Writer to encode to
     * @returns {$protobuf.Writer} Writer
     */
    EventData.encode = function encode(message, writer) {
        if (!writer)
            writer = $Writer.create();
        if (message.level != null && Object.hasOwnProperty.call(message, "level"))
            writer.uint32(/* id 1, wireType 0 =*/8).int32(message.level);
        if (message.timestampMs != null && Object.hasOwnProperty.call(message, "timestampMs"))
            writer.uint32(/* id 2, wireType 0 =*/16).uint64(message.timestampMs);
        if (message.uptimeMs != null && Object.hasOwnProperty.call(message, "uptimeMs"))
            writer.uint32(/* id 3, wireType 0 =*/24).uint64(message.uptimeMs);
        if (message.message != null && Object.hasOwnProperty.call(message, "message"))
            writer.uint32(/* id 4, wireType 2 =*/34).string(message.message);
        return writer;
    };

    /**
     * Encodes the specified EventData message, length delimited. Does not implicitly {@link EventData.verify|verify} messages.
     * @function encodeDelimited
     * @memberof EventData
     * @static
     * @param {IEventData} message EventData message or plain object to encode
     * @param {$protobuf.Writer} [writer] Writer to encode to
     * @returns {$protobuf.Writer} Writer
     */
    EventData.encodeDelimited = function encodeDelimited(message, writer) {
        return this.encode(message, writer).ldelim();
    };

    /**
     * Decodes an EventData message from the specified reader or buffer.
     * @function decode
     * @memberof EventData
     * @static
     * @param {$protobuf.Reader|Uint8Array} reader Reader or buffer to decode from
     * @param {number} [length] Message length if known beforehand
     * @returns {EventData} EventData
     * @throws {Error} If the payload is not a reader or valid buffer
     * @throws {$protobuf.util.ProtocolError} If required fields are missing
     */
    EventData.decode = function decode(reader, length, error) {
        if (!(reader instanceof $Reader))
            reader = $Reader.create(reader);
        let end = length === undefined ? reader.len : reader.pos + length, message = new $root.EventData();
        while (reader.pos < end) {
            let tag = reader.uint32();
            if (tag === error)
                break;
            switch (tag >>> 3) {
            case 1: {
                    message.level = reader.int32();
                    break;
                }
            case 2: {
                    message.timestampMs = reader.uint64();
                    break;
                }
            case 3: {
                    message.uptimeMs = reader.uint64();
                    break;
                }
            case 4: {
                    message.message = reader.string();
                    break;
                }
            default:
                reader.skipType(tag & 7);
                break;
            }
        }
        return message;
    };

    /**
     * Decodes an EventData message from the specified reader or buffer, length delimited.
     * @function decodeDelimited
     * @memberof EventData
     * @static
     * @param {$protobuf.Reader|Uint8Array} reader Reader or buffer to decode from
     * @returns {EventData} EventData
     * @throws {Error} If the payload is not a reader or valid buffer
     * @throws {$protobuf.util.ProtocolError} If required fields are missing
     */
    EventData.decodeDelimited = function decodeDelimited(reader) {
        if (!(reader instanceof $Reader))
            reader = new $Reader(reader);
        return this.decode(reader, reader.uint32());
    };

    /**
     * Verifies an EventData message.
     * @function verify
     * @memberof EventData
     * @static
     * @param {Object.<string,*>} message Plain object to verify
     * @returns {string|null} `null` if valid, otherwise the reason why it is not
     */
    EventData.verify = function verify(message) {
        if (typeof message !== "object" || message === null)
            return "object expected";
        if (message.level != null && message.hasOwnProperty("level"))
            if (!$util.isInteger(message.level))
                return "level: integer expected";
        if (message.timestampMs != null && message.hasOwnProperty("timestampMs"))
            if (!$util.isInteger(message.timestampMs) && !(message.timestampMs && $util.isInteger(message.timestampMs.low) && $util.isInteger(message.timestampMs.high)))
                return "timestampMs: integer|Long expected";
        if (message.uptimeMs != null && message.hasOwnProperty("uptimeMs"))
            if (!$util.isInteger(message.uptimeMs) && !(message.uptimeMs && $util.isInteger(message.uptimeMs.low) && $util.isInteger(message.uptimeMs.high)))
                return "uptimeMs: integer|Long expected";
        if (message.message != null && message.hasOwnProperty("message"))
            if (!$util.isString(message.message))
                return "message: string expected";
        return null;
    };

    /**
     * Creates an EventData message from a plain object. Also converts values to their respective internal types.
     * @function fromObject
     * @memberof EventData
     * @static
     * @param {Object.<string,*>} object Plain object
     * @returns {EventData} EventData
     */
    EventData.fromObject = function fromObject(object) {
        if (object instanceof $root.EventData)
            return object;
        let message = new $root.EventData();
        if (object.level != null)
            message.level = object.level | 0;
        if (object.timestampMs != null)
            if ($util.Long)
                (message.timestampMs = $util.Long.fromValue(object.timestampMs)).unsigned = true;
            else if (typeof object.timestampMs === "string")
                message.timestampMs = parseInt(object.timestampMs, 10);
            else if (typeof object.timestampMs === "number")
                message.timestampMs = object.timestampMs;
            else if (typeof object.timestampMs === "object")
                message.timestampMs = new $util.LongBits(object.timestampMs.low >>> 0, object.timestampMs.high >>> 0).toNumber(true);
        if (object.uptimeMs != null)
            if ($util.Long)
                (message.uptimeMs = $util.Long.fromValue(object.uptimeMs)).unsigned = true;
            else if (typeof object.uptimeMs === "string")
                message.uptimeMs = parseInt(object.uptimeMs, 10);
            else if (typeof object.uptimeMs === "number")
                message.uptimeMs = object.uptimeMs;
            else if (typeof object.uptimeMs === "object")
                message.uptimeMs = new $util.LongBits(object.uptimeMs.low >>> 0, object.uptimeMs.high >>> 0).toNumber(true);
        if (object.message != null)
            message.message = String(object.message);
        return message;
    };

    /**
     * Creates a plain object from an EventData message. Also converts values to other types if specified.
     * @function toObject
     * @memberof EventData
     * @static
     * @param {EventData} message EventData
     * @param {$protobuf.IConversionOptions} [options] Conversion options
     * @returns {Object.<string,*>} Plain object
     */
    EventData.toObject = function toObject(message, options) {
        if (!options)
            options = {};
        let object = {};
        if (options.defaults) {
            object.level = 0;
            if ($util.Long) {
                let long = new $util.Long(0, 0, true);
                object.timestampMs = options.longs === String ? long.toString() : options.longs === Number ? long.toNumber() : long;
            } else
                object.timestampMs = options.longs === String ? "0" : 0;
            if ($util.Long) {
                let long = new $util.Long(0, 0, true);
                object.uptimeMs = options.longs === String ? long.toString() : options.longs === Number ? long.toNumber() : long;
            } else
                object.uptimeMs = options.longs === String ? "0" : 0;
            object.message = "";
        }
        if (message.level != null && message.hasOwnProperty("level"))
            object.level = message.level;
        if (message.timestampMs != null && message.hasOwnProperty("timestampMs"))
            if (typeof message.timestampMs === "number")
                object.timestampMs = options.longs === String ? String(message.timestampMs) : message.timestampMs;
            else
                object.timestampMs = options.longs === String ? $util.Long.prototype.toString.call(message.timestampMs) : options.longs === Number ? new $util.LongBits(message.timestampMs.low >>> 0, message.timestampMs.high >>> 0).toNumber(true) : message.timestampMs;
        if (message.uptimeMs != null && message.hasOwnProperty("uptimeMs"))
            if (typeof message.uptimeMs === "number")
                object.uptimeMs = options.longs === String ? String(message.uptimeMs) : message.uptimeMs;
            else
                object.uptimeMs = options.longs === String ? $util.Long.prototype.toString.call(message.uptimeMs) : options.longs === Number ? new $util.LongBits(message.uptimeMs.low >>> 0, message.uptimeMs.high >>> 0).toNumber(true) : message.uptimeMs;
        if (message.message != null && message.hasOwnProperty("message"))
            object.message = message.message;
        return object;
    };

    /**
     * Converts this EventData to JSON.
     * @function toJSON
     * @memberof EventData
     * @instance
     * @returns {Object.<string,*>} JSON object
     */
    EventData.prototype.toJSON = function toJSON() {
        return this.constructor.toObject(this, $protobuf.util.toJSONOptions);
    };

    /**
     * Gets the default type url for EventData
     * @function getTypeUrl
     * @memberof EventData
     * @static
     * @param {string} [typeUrlPrefix] your custom typeUrlPrefix(default "type.googleapis.com")
     * @returns {string} The default type url
     */
    EventData.getTypeUrl = function getTypeUrl(typeUrlPrefix) {
        if (typeUrlPrefix === undefined) {
            typeUrlPrefix = "type.googleapis.com";
        }
        return typeUrlPrefix + "/EventData";
    };

    return EventData;
})();

export const UartData = $root.UartData = (() => {

    /**
     * Properties of an UartData.
     * @exports IUartData
     * @interface IUartData
     * @property {Uint8Array|null} [data] UartData data
     */

    /**
     * Constructs a new UartData.
     * @exports UartData
     * @classdesc Represents an UartData.
     * @implements IUartData
     * @constructor
     * @param {IUartData=} [properties] Properties to set
     */
    function UartData(properties) {
        if (properties)
            for (let keys = Object.keys(properties), i = 0; i < keys.length; ++i)
                if (properties[keys[i]] != null)
                    this[keys[i]] = properties[keys[i]];
    }

    /**
     * UartData data.
     * @member {Uint8Array} data
     * @memberof UartData
     * @instance
     */
    UartData.prototype.data = $util.newBuffer([]);

    /**
     * Creates a new UartData instance using the specified properties.
     * @function create
     * @memberof UartData
     * @static
     * @param {IUartData=} [properties] Properties to set
     * @returns {UartData} UartData instance
     */
    UartData.create = function create(properties) {
        return new UartData(properties);
    };

    /**
     * Encodes the specified UartData message. Does not implicitly {@link UartData.verify|verify} messages.
     * @function encode
     * @memberof UartData
     * @static
     * @param {IUartData} message UartData message or plain object to encode
     * @param {$protobuf.Writer} [writer] Writer to encode to
     * @returns {$protobuf.Writer} Writer
     */
    UartData.encode = function encode(message, writer) {
        if (!writer)
            writer = $Writer.create();
        if (message.data != null && Object.hasOwnProperty.call(message, "data"))
            writer.uint32(/* id 1, wireType 2 =*/10).bytes(message.data);
        return writer;
    };

    /**
     * Encodes the specified UartData message, length delimited. Does not implicitly {@link UartData.verify|verify} messages.
     * @function encodeDelimited
     * @memberof UartData
     * @static
     * @param {IUartData} message UartData message or plain object to encode
     * @param {$protobuf.Writer} [writer] Writer to encode to
     * @returns {$protobuf.Writer} Writer
     */
    UartData.encodeDelimited = function encodeDelimited(message, writer) {
        return this.encode(message, writer).ldelim();
    };

    /**
     * Decodes an UartData message from the specified reader or buffer.
     * @function decode
     * @memberof UartData
     * @static
     * @param {$protobuf.Reader|Uint8Array} reader Reader or buffer to decode from
     * @param {number} [length] Message length if known beforehand
     * @returns {UartData} UartData
     * @throws {Error} If the payload is not a reader or valid buffer
     * @throws {$protobuf.util.ProtocolError} If required fields are missing
     */
    UartData.decode = function decode(reader, length, error) {
        if (!(reader instanceof $Reader))
            reader = $Reader.create(reader);
        let end = length === undefined ? reader.len : reader.pos + length, message = new $root.UartData();
        while (reader.pos < end) {
            let tag = reader.uint32();
            if (tag === error)
                break;
            switch (tag >>> 3) {
            case 1: {
                    message.data = reader.bytes();
                    break;
                }
            default:
                reader.skipType(tag & 7);
                break;
            }
        }
        return message;
    };

    /**
     * Decodes an UartData message from the specified reader or buffer, length delimited.
     * @function decodeDelimited
     * @memberof UartData
     * @static
     * @param {$protobuf.Reader|Uint8Array} reader Reader or buffer to decode from
     * @returns {UartData} UartData
     * @throws {Error} If the payload is not a reader or valid buffer
     * @throws {$protobuf.util.ProtocolError} If required fields are missing
     */
    UartData.decodeDelimited = function decodeDelimited(reader) {
        if (!(reader instanceof $Reader))
            reader = new $Reader(reader);
        return this.decode(reader, reader.uint32());
    };

    /**
     * Verifies an UartData message.
     * @function verify
     * @memberof UartData
     * @static
     * @param {Object.<string,*>} message Plain object to verify
     * @returns {string|null} `null` if valid, otherwise the reason why it is not
     */
    UartData.verify = function verify(message) {
        if (typeof message !== "object" || message === null)
            return "object expected";
        if (message.data != null && message.hasOwnProperty("data"))
            if (!(message.data && typeof message.data.length === "number" || $util.isString(message.data)))
                return "data: buffer expected";
        return null;
    };

    /**
     * Creates an UartData message from a plain object. Also converts values to their respective internal types.
     * @function fromObject
     * @memberof UartData
     * @static
     * @param {Object.<string,*>} object Plain object
     * @returns {UartData} UartData
     */
    UartData.fromObject = function fromObject(object) {
        if (object instanceof $root.UartData)
            return object;
        let message = new $root.UartData();
        if (object.data != null)
            if (typeof object.data === "string")
                $util.base64.decode(object.data, message.data = $util.newBuffer($util.base64.length(object.data)), 0);
            else if (object.data.length >= 0)
                message.data = object.data;
        return message;
    };

    /**
     * Creates a plain object from an UartData message. Also converts values to other types if specified.
     * @function toObject
     * @memberof UartData
     * @static
     * @param {UartData} message UartData
     * @param {$protobuf.IConversionOptions} [options] Conversion options
     * @returns {Object.<string,*>} Plain object
     */
    UartData.toObject = function toObject(message, options) {
        if (!options)
            options = {};
        let object = {};
        if (options.defaults)
            if (options.bytes === String)
                object.data = "";
            else {
                object.data = [];
                if (options.bytes !== Array)
                    object.data = $util.newBuffer(object.data);
            }
        if (message.data != null && message.hasOwnProperty("data"))
            object.data = options.bytes === String ? $util.base64.encode(message.data, 0, message.data.length) : options.bytes === Array ? Array.prototype.slice.call(message.data) : message.data;
        return object;
    };

    /**
     * Converts this UartData to JSON.
     * @function toJSON
     * @memberof UartData
     * @instance
     * @returns {Object.<string,*>} JSON object
     */
    UartData.prototype.toJSON = function toJSON() {
        return this.constructor.toObject(this, $protobuf.util.toJSONOptions);
    };

    /**
     * Gets the default type url for UartData
     * @function getTypeUrl
     * @memberof UartData
     * @static
     * @param {string} [typeUrlPrefix] your custom typeUrlPrefix(default "type.googleapis.com")
     * @returns {string} The default type url
     */
    UartData.getTypeUrl = function getTypeUrl(typeUrlPrefix) {
        if (typeUrlPrefix === undefined) {
            typeUrlPrefix = "type.googleapis.com";
        }
        return typeUrlPrefix + "/UartData";
    };

    return UartData;
})();

export const LoadSwStatus = $root.LoadSwStatus = (() => {

    /**
     * Properties of a LoadSwStatus.
     * @exports ILoadSwStatus
     * @interface ILoadSwStatus
     * @property {boolean|null} [main] LoadSwStatus main
     * @property {boolean|null} [usb] LoadSwStatus usb
     */

    /**
     * Constructs a new LoadSwStatus.
     * @exports LoadSwStatus
     * @classdesc Represents a LoadSwStatus.
     * @implements ILoadSwStatus
     * @constructor
     * @param {ILoadSwStatus=} [properties] Properties to set
     */
    function LoadSwStatus(properties) {
        if (properties)
            for (let keys = Object.keys(properties), i = 0; i < keys.length; ++i)
                if (properties[keys[i]] != null)
                    this[keys[i]] = properties[keys[i]];
    }

    /**
     * LoadSwStatus main.
     * @member {boolean} main
     * @memberof LoadSwStatus
     * @instance
     */
    LoadSwStatus.prototype.main = false;

    /**
     * LoadSwStatus usb.
     * @member {boolean} usb
     * @memberof LoadSwStatus
     * @instance
     */
    LoadSwStatus.prototype.usb = false;

    /**
     * Creates a new LoadSwStatus instance using the specified properties.
     * @function create
     * @memberof LoadSwStatus
     * @static
     * @param {ILoadSwStatus=} [properties] Properties to set
     * @returns {LoadSwStatus} LoadSwStatus instance
     */
    LoadSwStatus.create = function create(properties) {
        return new LoadSwStatus(properties);
    };

    /**
     * Encodes the specified LoadSwStatus message. Does not implicitly {@link LoadSwStatus.verify|verify} messages.
     * @function encode
     * @memberof LoadSwStatus
     * @static
     * @param {ILoadSwStatus} message LoadSwStatus message or plain object to encode
     * @param {$protobuf.Writer} [writer] Writer to encode to
     * @returns {$protobuf.Writer} Writer
     */
    LoadSwStatus.encode = function encode(message, writer) {
        if (!writer)
            writer = $Writer.create();
        if (message.main != null && Object.hasOwnProperty.call(message, "main"))
            writer.uint32(/* id 1, wireType 0 =*/8).bool(message.main);
        if (message.usb != null && Object.hasOwnProperty.call(message, "usb"))
            writer.uint32(/* id 2, wireType 0 =*/16).bool(message.usb);
        return writer;
    };

    /**
     * Encodes the specified LoadSwStatus message, length delimited. Does not implicitly {@link LoadSwStatus.verify|verify} messages.
     * @function encodeDelimited
     * @memberof LoadSwStatus
     * @static
     * @param {ILoadSwStatus} message LoadSwStatus message or plain object to encode
     * @param {$protobuf.Writer} [writer] Writer to encode to
     * @returns {$protobuf.Writer} Writer
     */
    LoadSwStatus.encodeDelimited = function encodeDelimited(message, writer) {
        return this.encode(message, writer).ldelim();
    };

    /**
     * Decodes a LoadSwStatus message from the specified reader or buffer.
     * @function decode
     * @memberof LoadSwStatus
     * @static
     * @param {$protobuf.Reader|Uint8Array} reader Reader or buffer to decode from
     * @param {number} [length] Message length if known beforehand
     * @returns {LoadSwStatus} LoadSwStatus
     * @throws {Error} If the payload is not a reader or valid buffer
     * @throws {$protobuf.util.ProtocolError} If required fields are missing
     */
    LoadSwStatus.decode = function decode(reader, length, error) {
        if (!(reader instanceof $Reader))
            reader = $Reader.create(reader);
        let end = length === undefined ? reader.len : reader.pos + length, message = new $root.LoadSwStatus();
        while (reader.pos < end) {
            let tag = reader.uint32();
            if (tag === error)
                break;
            switch (tag >>> 3) {
            case 1: {
                    message.main = reader.bool();
                    break;
                }
            case 2: {
                    message.usb = reader.bool();
                    break;
                }
            default:
                reader.skipType(tag & 7);
                break;
            }
        }
        return message;
    };

    /**
     * Decodes a LoadSwStatus message from the specified reader or buffer, length delimited.
     * @function decodeDelimited
     * @memberof LoadSwStatus
     * @static
     * @param {$protobuf.Reader|Uint8Array} reader Reader or buffer to decode from
     * @returns {LoadSwStatus} LoadSwStatus
     * @throws {Error} If the payload is not a reader or valid buffer
     * @throws {$protobuf.util.ProtocolError} If required fields are missing
     */
    LoadSwStatus.decodeDelimited = function decodeDelimited(reader) {
        if (!(reader instanceof $Reader))
            reader = new $Reader(reader);
        return this.decode(reader, reader.uint32());
    };

    /**
     * Verifies a LoadSwStatus message.
     * @function verify
     * @memberof LoadSwStatus
     * @static
     * @param {Object.<string,*>} message Plain object to verify
     * @returns {string|null} `null` if valid, otherwise the reason why it is not
     */
    LoadSwStatus.verify = function verify(message) {
        if (typeof message !== "object" || message === null)
            return "object expected";
        if (message.main != null && message.hasOwnProperty("main"))
            if (typeof message.main !== "boolean")
                return "main: boolean expected";
        if (message.usb != null && message.hasOwnProperty("usb"))
            if (typeof message.usb !== "boolean")
                return "usb: boolean expected";
        return null;
    };

    /**
     * Creates a LoadSwStatus message from a plain object. Also converts values to their respective internal types.
     * @function fromObject
     * @memberof LoadSwStatus
     * @static
     * @param {Object.<string,*>} object Plain object
     * @returns {LoadSwStatus} LoadSwStatus
     */
    LoadSwStatus.fromObject = function fromObject(object) {
        if (object instanceof $root.LoadSwStatus)
            return object;
        let message = new $root.LoadSwStatus();
        if (object.main != null)
            message.main = Boolean(object.main);
        if (object.usb != null)
            message.usb = Boolean(object.usb);
        return message;
    };

    /**
     * Creates a plain object from a LoadSwStatus message. Also converts values to other types if specified.
     * @function toObject
     * @memberof LoadSwStatus
     * @static
     * @param {LoadSwStatus} message LoadSwStatus
     * @param {$protobuf.IConversionOptions} [options] Conversion options
     * @returns {Object.<string,*>} Plain object
     */
    LoadSwStatus.toObject = function toObject(message, options) {
        if (!options)
            options = {};
        let object = {};
        if (options.defaults) {
            object.main = false;
            object.usb = false;
        }
        if (message.main != null && message.hasOwnProperty("main"))
            object.main = message.main;
        if (message.usb != null && message.hasOwnProperty("usb"))
            object.usb = message.usb;
        return object;
    };

    /**
     * Converts this LoadSwStatus to JSON.
     * @function toJSON
     * @memberof LoadSwStatus
     * @instance
     * @returns {Object.<string,*>} JSON object
     */
    LoadSwStatus.prototype.toJSON = function toJSON() {
        return this.constructor.toObject(this, $protobuf.util.toJSONOptions);
    };

    /**
     * Gets the default type url for LoadSwStatus
     * @function getTypeUrl
     * @memberof LoadSwStatus
     * @static
     * @param {string} [typeUrlPrefix] your custom typeUrlPrefix(default "type.googleapis.com")
     * @returns {string} The default type url
     */
    LoadSwStatus.getTypeUrl = function getTypeUrl(typeUrlPrefix) {
        if (typeUrlPrefix === undefined) {
            typeUrlPrefix = "type.googleapis.com";
        }
        return typeUrlPrefix + "/LoadSwStatus";
    };

    return LoadSwStatus;
})();

export const StatusMessage = $root.StatusMessage = (() => {

    /**
     * Properties of a StatusMessage.
     * @exports IStatusMessage
     * @interface IStatusMessage
     * @property {ISensorData|null} [sensorData] StatusMessage sensorData
     * @property {IWifiStatus|null} [wifiStatus] StatusMessage wifiStatus
     * @property {ILoadSwStatus|null} [swStatus] StatusMessage swStatus
     * @property {IUartData|null} [uartData] StatusMessage uartData
     * @property {IEventData|null} [eventData] StatusMessage eventData
     */

    /**
     * Constructs a new StatusMessage.
     * @exports StatusMessage
     * @classdesc Represents a StatusMessage.
     * @implements IStatusMessage
     * @constructor
     * @param {IStatusMessage=} [properties] Properties to set
     */
    function StatusMessage(properties) {
        if (properties)
            for (let keys = Object.keys(properties), i = 0; i < keys.length; ++i)
                if (properties[keys[i]] != null)
                    this[keys[i]] = properties[keys[i]];
    }

    /**
     * StatusMessage sensorData.
     * @member {ISensorData|null|undefined} sensorData
     * @memberof StatusMessage
     * @instance
     */
    StatusMessage.prototype.sensorData = null;

    /**
     * StatusMessage wifiStatus.
     * @member {IWifiStatus|null|undefined} wifiStatus
     * @memberof StatusMessage
     * @instance
     */
    StatusMessage.prototype.wifiStatus = null;

    /**
     * StatusMessage swStatus.
     * @member {ILoadSwStatus|null|undefined} swStatus
     * @memberof StatusMessage
     * @instance
     */
    StatusMessage.prototype.swStatus = null;

    /**
     * StatusMessage uartData.
     * @member {IUartData|null|undefined} uartData
     * @memberof StatusMessage
     * @instance
     */
    StatusMessage.prototype.uartData = null;

    /**
     * StatusMessage eventData.
     * @member {IEventData|null|undefined} eventData
     * @memberof StatusMessage
     * @instance
     */
    StatusMessage.prototype.eventData = null;

    // OneOf field names bound to virtual getters and setters
    let $oneOfFields;

    /**
     * StatusMessage payload.
     * @member {"sensorData"|"wifiStatus"|"swStatus"|"uartData"|"eventData"|undefined} payload
     * @memberof StatusMessage
     * @instance
     */
    Object.defineProperty(StatusMessage.prototype, "payload", {
        get: $util.oneOfGetter($oneOfFields = ["sensorData", "wifiStatus", "swStatus", "uartData", "eventData"]),
        set: $util.oneOfSetter($oneOfFields)
    });

    /**
     * Creates a new StatusMessage instance using the specified properties.
     * @function create
     * @memberof StatusMessage
     * @static
     * @param {IStatusMessage=} [properties] Properties to set
     * @returns {StatusMessage} StatusMessage instance
     */
    StatusMessage.create = function create(properties) {
        return new StatusMessage(properties);
    };

    /**
     * Encodes the specified StatusMessage message. Does not implicitly {@link StatusMessage.verify|verify} messages.
     * @function encode
     * @memberof StatusMessage
     * @static
     * @param {IStatusMessage} message StatusMessage message or plain object to encode
     * @param {$protobuf.Writer} [writer] Writer to encode to
     * @returns {$protobuf.Writer} Writer
     */
    StatusMessage.encode = function encode(message, writer) {
        if (!writer)
            writer = $Writer.create();
        if (message.sensorData != null && Object.hasOwnProperty.call(message, "sensorData"))
            $root.SensorData.encode(message.sensorData, writer.uint32(/* id 1, wireType 2 =*/10).fork()).ldelim();
        if (message.wifiStatus != null && Object.hasOwnProperty.call(message, "wifiStatus"))
            $root.WifiStatus.encode(message.wifiStatus, writer.uint32(/* id 2, wireType 2 =*/18).fork()).ldelim();
        if (message.swStatus != null && Object.hasOwnProperty.call(message, "swStatus"))
            $root.LoadSwStatus.encode(message.swStatus, writer.uint32(/* id 3, wireType 2 =*/26).fork()).ldelim();
        if (message.uartData != null && Object.hasOwnProperty.call(message, "uartData"))
            $root.UartData.encode(message.uartData, writer.uint32(/* id 4, wireType 2 =*/34).fork()).ldelim();
        if (message.eventData != null && Object.hasOwnProperty.call(message, "eventData"))
            $root.EventData.encode(message.eventData, writer.uint32(/* id 5, wireType 2 =*/42).fork()).ldelim();
        return writer;
    };

    /**
     * Encodes the specified StatusMessage message, length delimited. Does not implicitly {@link StatusMessage.verify|verify} messages.
     * @function encodeDelimited
     * @memberof StatusMessage
     * @static
     * @param {IStatusMessage} message StatusMessage message or plain object to encode
     * @param {$protobuf.Writer} [writer] Writer to encode to
     * @returns {$protobuf.Writer} Writer
     */
    StatusMessage.encodeDelimited = function encodeDelimited(message, writer) {
        return this.encode(message, writer).ldelim();
    };

    /**
     * Decodes a StatusMessage message from the specified reader or buffer.
     * @function decode
     * @memberof StatusMessage
     * @static
     * @param {$protobuf.Reader|Uint8Array} reader Reader or buffer to decode from
     * @param {number} [length] Message length if known beforehand
     * @returns {StatusMessage} StatusMessage
     * @throws {Error} If the payload is not a reader or valid buffer
     * @throws {$protobuf.util.ProtocolError} If required fields are missing
     */
    StatusMessage.decode = function decode(reader, length, error) {
        if (!(reader instanceof $Reader))
            reader = $Reader.create(reader);
        let end = length === undefined ? reader.len : reader.pos + length, message = new $root.StatusMessage();
        while (reader.pos < end) {
            let tag = reader.uint32();
            if (tag === error)
                break;
            switch (tag >>> 3) {
            case 1: {
                    message.sensorData = $root.SensorData.decode(reader, reader.uint32());
                    break;
                }
            case 2: {
                    message.wifiStatus = $root.WifiStatus.decode(reader, reader.uint32());
                    break;
                }
            case 3: {
                    message.swStatus = $root.LoadSwStatus.decode(reader, reader.uint32());
                    break;
                }
            case 4: {
                    message.uartData = $root.UartData.decode(reader, reader.uint32());
                    break;
                }
            case 5: {
                    message.eventData = $root.EventData.decode(reader, reader.uint32());
                    break;
                }
            default:
                reader.skipType(tag & 7);
                break;
            }
        }
        return message;
    };

    /**
     * Decodes a StatusMessage message from the specified reader or buffer, length delimited.
     * @function decodeDelimited
     * @memberof StatusMessage
     * @static
     * @param {$protobuf.Reader|Uint8Array} reader Reader or buffer to decode from
     * @returns {StatusMessage} StatusMessage
     * @throws {Error} If the payload is not a reader or valid buffer
     * @throws {$protobuf.util.ProtocolError} If required fields are missing
     */
    StatusMessage.decodeDelimited = function decodeDelimited(reader) {
        if (!(reader instanceof $Reader))
            reader = new $Reader(reader);
        return this.decode(reader, reader.uint32());
    };

    /**
     * Verifies a StatusMessage message.
     * @function verify
     * @memberof StatusMessage
     * @static
     * @param {Object.<string,*>} message Plain object to verify
     * @returns {string|null} `null` if valid, otherwise the reason why it is not
     */
    StatusMessage.verify = function verify(message) {
        if (typeof message !== "object" || message === null)
            return "object expected";
        let properties = {};
        if (message.sensorData != null && message.hasOwnProperty("sensorData")) {
            properties.payload = 1;
            {
                let error = $root.SensorData.verify(message.sensorData);
                if (error)
                    return "sensorData." + error;
            }
        }
        if (message.wifiStatus != null && message.hasOwnProperty("wifiStatus")) {
            if (properties.payload === 1)
                return "payload: multiple values";
            properties.payload = 1;
            {
                let error = $root.WifiStatus.verify(message.wifiStatus);
                if (error)
                    return "wifiStatus." + error;
            }
        }
        if (message.swStatus != null && message.hasOwnProperty("swStatus")) {
            if (properties.payload === 1)
                return "payload: multiple values";
            properties.payload = 1;
            {
                let error = $root.LoadSwStatus.verify(message.swStatus);
                if (error)
                    return "swStatus." + error;
            }
        }
        if (message.uartData != null && message.hasOwnProperty("uartData")) {
            if (properties.payload === 1)
                return "payload: multiple values";
            properties.payload = 1;
            {
                let error = $root.UartData.verify(message.uartData);
                if (error)
                    return "uartData." + error;
            }
        }
        if (message.eventData != null && message.hasOwnProperty("eventData")) {
            if (properties.payload === 1)
                return "payload: multiple values";
            properties.payload = 1;
            {
                let error = $root.EventData.verify(message.eventData);
                if (error)
                    return "eventData." + error;
            }
        }
        return null;
    };

    /**
     * Creates a StatusMessage message from a plain object. Also converts values to their respective internal types.
     * @function fromObject
     * @memberof StatusMessage
     * @static
     * @param {Object.<string,*>} object Plain object
     * @returns {StatusMessage} StatusMessage
     */
    StatusMessage.fromObject = function fromObject(object) {
        if (object instanceof $root.StatusMessage)
            return object;
        let message = new $root.StatusMessage();
        if (object.sensorData != null) {
            if (typeof object.sensorData !== "object")
                throw TypeError(".StatusMessage.sensorData: object expected");
            message.sensorData = $root.SensorData.fromObject(object.sensorData);
        }
        if (object.wifiStatus != null) {
            if (typeof object.wifiStatus !== "object")
                throw TypeError(".StatusMessage.wifiStatus: object expected");
            message.wifiStatus = $root.WifiStatus.fromObject(object.wifiStatus);
        }
        if (object.swStatus != null) {
            if (typeof object.swStatus !== "object")
                throw TypeError(".StatusMessage.swStatus: object expected");
            message.swStatus = $root.LoadSwStatus.fromObject(object.swStatus);
        }
        if (object.uartData != null) {
            if (typeof object.uartData !== "object")
                throw TypeError(".StatusMessage.uartData: object expected");
            message.uartData = $root.UartData.fromObject(object.uartData);
        }
        if (object.eventData != null) {
            if (typeof object.eventData !== "object")
                throw TypeError(".StatusMessage.eventData: object expected");
            message.eventData = $root.EventData.fromObject(object.eventData);
        }
        return message;
    };

    /**
     * Creates a plain object from a StatusMessage message. Also converts values to other types if specified.
     * @function toObject
     * @memberof StatusMessage
     * @static
     * @param {StatusMessage} message StatusMessage
     * @param {$protobuf.IConversionOptions} [options] Conversion options
     * @returns {Object.<string,*>} Plain object
     */
    StatusMessage.toObject = function toObject(message, options) {
        if (!options)
            options = {};
        let object = {};
        if (message.sensorData != null && message.hasOwnProperty("sensorData")) {
            object.sensorData = $root.SensorData.toObject(message.sensorData, options);
            if (options.oneofs)
                object.payload = "sensorData";
        }
        if (message.wifiStatus != null && message.hasOwnProperty("wifiStatus")) {
            object.wifiStatus = $root.WifiStatus.toObject(message.wifiStatus, options);
            if (options.oneofs)
                object.payload = "wifiStatus";
        }
        if (message.swStatus != null && message.hasOwnProperty("swStatus")) {
            object.swStatus = $root.LoadSwStatus.toObject(message.swStatus, options);
            if (options.oneofs)
                object.payload = "swStatus";
        }
        if (message.uartData != null && message.hasOwnProperty("uartData")) {
            object.uartData = $root.UartData.toObject(message.uartData, options);
            if (options.oneofs)
                object.payload = "uartData";
        }
        if (message.eventData != null && message.hasOwnProperty("eventData")) {
            object.eventData = $root.EventData.toObject(message.eventData, options);
            if (options.oneofs)
                object.payload = "eventData";
        }
        return object;
    };

    /**
     * Converts this StatusMessage to JSON.
     * @function toJSON
     * @memberof StatusMessage
     * @instance
     * @returns {Object.<string,*>} JSON object
     */
    StatusMessage.prototype.toJSON = function toJSON() {
        return this.constructor.toObject(this, $protobuf.util.toJSONOptions);
    };

    /**
     * Gets the default type url for StatusMessage
     * @function getTypeUrl
     * @memberof StatusMessage
     * @static
     * @param {string} [typeUrlPrefix] your custom typeUrlPrefix(default "type.googleapis.com")
     * @returns {string} The default type url
     */
    StatusMessage.getTypeUrl = function getTypeUrl(typeUrlPrefix) {
        if (typeUrlPrefix === undefined) {
            typeUrlPrefix = "type.googleapis.com";
        }
        return typeUrlPrefix + "/StatusMessage";
    };

    return StatusMessage;
})();

export { $root as default };
