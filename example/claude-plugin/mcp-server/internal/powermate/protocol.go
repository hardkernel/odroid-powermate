// Derived from example/pmtui/protocol.go: hand-written protowire decoder for proto/status.proto.

package powermate

import (
	"fmt"
	"math"

	"google.golang.org/protobuf/encoding/protowire"
)

type PayloadKind uint8

const (
	PayloadUnknown PayloadKind = iota
	PayloadSensor
	PayloadWiFi
	PayloadSwitch
	PayloadUART
	PayloadEvent
)

type ChannelData struct {
	Voltage float32
	Current float32
	Power   float32
}

type SensorData struct {
	USB         ChannelData
	Main        ChannelData
	VIN         ChannelData
	TimestampMS uint64
	UptimeMS    uint64
}

type WiFiStatus struct {
	Connected bool
	SSID      string
	RSSI      int32
	IPAddress string
}

type SwitchStatus struct {
	Main bool
	USB  bool
}

type EventData struct {
	Level       int32
	TimestampMS uint64
	UptimeMS    uint64
	Message     string
}

type StatusMessage struct {
	Kind   PayloadKind
	Sensor SensorData
	WiFi   WiFiStatus
	Switch SwitchStatus
	UART   []byte
	Event  EventData
}

func DecodeStatusMessage(data []byte) (StatusMessage, error) {
	var message StatusMessage

	for len(data) > 0 {
		number, wireType, tagLen := protowire.ConsumeTag(data)
		if tagLen < 0 {
			return message, protowire.ParseError(tagLen)
		}
		data = data[tagLen:]

		if wireType != protowire.BytesType {
			consumed := protowire.ConsumeFieldValue(number, wireType, data)
			if consumed < 0 {
				return message, protowire.ParseError(consumed)
			}
			data = data[consumed:]
			continue
		}

		value, consumed := protowire.ConsumeBytes(data)
		if consumed < 0 {
			return message, protowire.ParseError(consumed)
		}
		data = data[consumed:]

		var err error
		switch number {
		case 1:
			message.Kind = PayloadSensor
			message.Sensor, err = decodeSensorData(value)
		case 2:
			message.Kind = PayloadWiFi
			message.WiFi, err = decodeWiFiStatus(value)
		case 3:
			message.Kind = PayloadSwitch
			message.Switch, err = decodeSwitchStatus(value)
		case 4:
			message.Kind = PayloadUART
			message.UART = append(message.UART[:0], value...)
		case 5:
			message.Kind = PayloadEvent
			message.Event, err = decodeEventData(value)
		default:
			continue
		}
		if err != nil {
			return message, err
		}
	}

	return message, nil
}

func decodeSensorData(data []byte) (SensorData, error) {
	var sensor SensorData

	for len(data) > 0 {
		number, wireType, tagLen := protowire.ConsumeTag(data)
		if tagLen < 0 {
			return sensor, protowire.ParseError(tagLen)
		}
		data = data[tagLen:]

		switch number {
		case 1, 2, 3:
			if wireType != protowire.BytesType {
				return sensor, unexpectedWireType(number, wireType)
			}
			value, consumed := protowire.ConsumeBytes(data)
			if consumed < 0 {
				return sensor, protowire.ParseError(consumed)
			}
			data = data[consumed:]
			channel, err := decodeChannelData(value)
			if err != nil {
				return sensor, err
			}
			switch number {
			case 1:
				sensor.USB = channel
			case 2:
				sensor.Main = channel
			case 3:
				sensor.VIN = channel
			}
		case 4, 5:
			if wireType != protowire.VarintType {
				return sensor, unexpectedWireType(number, wireType)
			}
			value, consumed := protowire.ConsumeVarint(data)
			if consumed < 0 {
				return sensor, protowire.ParseError(consumed)
			}
			data = data[consumed:]
			if number == 4 {
				sensor.TimestampMS = value
			} else {
				sensor.UptimeMS = value
			}
		default:
			consumed := protowire.ConsumeFieldValue(number, wireType, data)
			if consumed < 0 {
				return sensor, protowire.ParseError(consumed)
			}
			data = data[consumed:]
		}
	}

	return sensor, nil
}

func decodeChannelData(data []byte) (ChannelData, error) {
	var channel ChannelData

	for len(data) > 0 {
		number, wireType, tagLen := protowire.ConsumeTag(data)
		if tagLen < 0 {
			return channel, protowire.ParseError(tagLen)
		}
		data = data[tagLen:]

		if number < 1 || number > 3 {
			consumed := protowire.ConsumeFieldValue(number, wireType, data)
			if consumed < 0 {
				return channel, protowire.ParseError(consumed)
			}
			data = data[consumed:]
			continue
		}
		if wireType != protowire.Fixed32Type {
			return channel, unexpectedWireType(number, wireType)
		}

		value, consumed := protowire.ConsumeFixed32(data)
		if consumed < 0 {
			return channel, protowire.ParseError(consumed)
		}
		data = data[consumed:]
		decoded := math.Float32frombits(value)
		switch number {
		case 1:
			channel.Voltage = decoded
		case 2:
			channel.Current = decoded
		case 3:
			channel.Power = decoded
		}
	}

	return channel, nil
}

func decodeWiFiStatus(data []byte) (WiFiStatus, error) {
	var status WiFiStatus

	for len(data) > 0 {
		number, wireType, tagLen := protowire.ConsumeTag(data)
		if tagLen < 0 {
			return status, protowire.ParseError(tagLen)
		}
		data = data[tagLen:]

		switch number {
		case 1, 3:
			if wireType != protowire.VarintType {
				return status, unexpectedWireType(number, wireType)
			}
			value, consumed := protowire.ConsumeVarint(data)
			if consumed < 0 {
				return status, protowire.ParseError(consumed)
			}
			data = data[consumed:]
			if number == 1 {
				status.Connected = value != 0
			} else {
				status.RSSI = int32(value)
			}
		case 2, 4:
			if wireType != protowire.BytesType {
				return status, unexpectedWireType(number, wireType)
			}
			value, consumed := protowire.ConsumeBytes(data)
			if consumed < 0 {
				return status, protowire.ParseError(consumed)
			}
			data = data[consumed:]
			if number == 2 {
				status.SSID = string(value)
			} else {
				status.IPAddress = string(value)
			}
		default:
			consumed := protowire.ConsumeFieldValue(number, wireType, data)
			if consumed < 0 {
				return status, protowire.ParseError(consumed)
			}
			data = data[consumed:]
		}
	}

	return status, nil
}

func decodeSwitchStatus(data []byte) (SwitchStatus, error) {
	var status SwitchStatus

	for len(data) > 0 {
		number, wireType, tagLen := protowire.ConsumeTag(data)
		if tagLen < 0 {
			return status, protowire.ParseError(tagLen)
		}
		data = data[tagLen:]

		if (number == 1 || number == 2) && wireType == protowire.VarintType {
			value, consumed := protowire.ConsumeVarint(data)
			if consumed < 0 {
				return status, protowire.ParseError(consumed)
			}
			data = data[consumed:]
			if number == 1 {
				status.Main = value != 0
			} else {
				status.USB = value != 0
			}
			continue
		}

		consumed := protowire.ConsumeFieldValue(number, wireType, data)
		if consumed < 0 {
			return status, protowire.ParseError(consumed)
		}
		data = data[consumed:]
	}

	return status, nil
}

func decodeEventData(data []byte) (EventData, error) {
	var event EventData

	for len(data) > 0 {
		number, wireType, tagLen := protowire.ConsumeTag(data)
		if tagLen < 0 {
			return event, protowire.ParseError(tagLen)
		}
		data = data[tagLen:]

		switch number {
		case 1, 2, 3:
			if wireType != protowire.VarintType {
				return event, unexpectedWireType(number, wireType)
			}
			value, consumed := protowire.ConsumeVarint(data)
			if consumed < 0 {
				return event, protowire.ParseError(consumed)
			}
			data = data[consumed:]
			switch number {
			case 1:
				event.Level = int32(value)
			case 2:
				event.TimestampMS = value
			case 3:
				event.UptimeMS = value
			}
		case 4:
			if wireType != protowire.BytesType {
				return event, unexpectedWireType(number, wireType)
			}
			value, consumed := protowire.ConsumeBytes(data)
			if consumed < 0 {
				return event, protowire.ParseError(consumed)
			}
			data = data[consumed:]
			event.Message = string(value)
		default:
			consumed := protowire.ConsumeFieldValue(number, wireType, data)
			if consumed < 0 {
				return event, protowire.ParseError(consumed)
			}
			data = data[consumed:]
		}
	}

	return event, nil
}

func unexpectedWireType(number protowire.Number, wireType protowire.Type) error {
	return fmt.Errorf("protobuf field %d has unexpected wire type %d", number, wireType)
}
