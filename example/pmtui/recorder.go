package main

import (
	"encoding/csv"
	"errors"
	"fmt"
	"os"
	"strconv"
	"sync"
	"time"
)

type recorder struct {
	mu       sync.Mutex
	file     *os.File
	writer   *csv.Writer
	filename string
}

func (r *recorder) Start(filename string) error {
	r.mu.Lock()
	defer r.mu.Unlock()

	if r.file != nil {
		return errors.New("recording is already active")
	}

	file, err := os.OpenFile(filename, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0o644)
	if err != nil {
		return err
	}

	writer := csv.NewWriter(file)
	if err := writer.Write([]string{
		"timestamp", "host_timestamp", "uptime_ms",
		"vin_voltage", "vin_current", "vin_power",
		"main_voltage", "main_current", "main_power",
		"usb_voltage", "usb_current", "usb_power",
	}); err != nil {
		file.Close()
		return err
	}
	writer.Flush()
	if err := writer.Error(); err != nil {
		file.Close()
		return err
	}

	r.file = file
	r.writer = writer
	r.filename = filename
	return nil
}

func (r *recorder) Stop() (string, error) {
	r.mu.Lock()
	defer r.mu.Unlock()

	if r.file == nil {
		return "", errors.New("recording is not active")
	}

	filename := r.filename
	r.writer.Flush()
	writerErr := r.writer.Error()
	closeErr := r.file.Close()
	r.file = nil
	r.writer = nil
	r.filename = ""

	if writerErr != nil {
		return filename, writerErr
	}
	return filename, closeErr
}

func (r *recorder) Active() bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.file != nil
}

func (r *recorder) Write(sensor sensorData) error {
	r.mu.Lock()
	defer r.mu.Unlock()

	if r.file == nil {
		return nil
	}

	hostTimestamp := time.Now().UTC()
	deviceTimestamp := ""
	if sensor.TimestampMS > 0 {
		deviceTimestamp = time.UnixMilli(int64(sensor.TimestampMS)).UTC().Format(time.RFC3339Nano)
	}

	row := []string{
		deviceTimestamp,
		hostTimestamp.Format(time.RFC3339Nano),
		strconv.FormatUint(sensor.UptimeMS, 10),
		formatFloat(sensor.VIN.Voltage),
		formatFloat(sensor.VIN.Current),
		formatFloat(sensor.VIN.Power),
		formatFloat(sensor.Main.Voltage),
		formatFloat(sensor.Main.Current),
		formatFloat(sensor.Main.Power),
		formatFloat(sensor.USB.Voltage),
		formatFloat(sensor.USB.Current),
		formatFloat(sensor.USB.Power),
	}
	if err := r.writer.Write(row); err != nil {
		return err
	}
	r.writer.Flush()
	return r.writer.Error()
}

func defaultRecordingFilename() string {
	return fmt.Sprintf("powermate_%s.csv", time.Now().Format("20060102_150405"))
}

func formatFloat(value float32) string {
	return strconv.FormatFloat(float64(value), 'f', 3, 32)
}
