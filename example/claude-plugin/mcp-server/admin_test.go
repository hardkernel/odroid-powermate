// SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
// SPDX-License-Identifier: Apache-2.0

package main

import "testing"

func TestValidateStatic(t *testing.T) {
	ok := networkIn{Mode: "static", IP: "192.168.30.50", Gateway: "192.168.30.1", Subnet: "255.255.255.0", DNS1: "8.8.8.8"}
	if err := validateStatic(ok); err != nil {
		t.Fatalf("valid config rejected: %v", err)
	}
	bad := map[string]networkIn{
		"gateway outside subnet": {IP: "192.168.30.50", Gateway: "192.168.31.1", Subnet: "255.255.255.0", DNS1: "8.8.8.8"},
		"broadcast address":      {IP: "192.168.30.255", Gateway: "192.168.30.1", Subnet: "255.255.255.0", DNS1: "8.8.8.8"},
		"network address":        {IP: "192.168.30.0", Gateway: "192.168.30.1", Subnet: "255.255.255.0", DNS1: "8.8.8.8"},
		"non-contiguous mask":    {IP: "192.168.30.50", Gateway: "192.168.30.1", Subnet: "255.0.255.0", DNS1: "8.8.8.8"},
		"ip equals gateway":      {IP: "192.168.30.1", Gateway: "192.168.30.1", Subnet: "255.255.255.0", DNS1: "8.8.8.8"},
		"not an address":         {IP: "192.168.30", Gateway: "192.168.30.1", Subnet: "255.255.255.0", DNS1: "8.8.8.8"},
		"ipv6":                   {IP: "fe80::1", Gateway: "192.168.30.1", Subnet: "255.255.255.0", DNS1: "8.8.8.8"},
	}
	for name, in := range bad {
		if err := validateStatic(in); err == nil {
			t.Errorf("%s accepted", name)
		}
	}
}

func TestValidateWiFi(t *testing.T) {
	if validateWiFi("lab", "") != nil || validateWiFi("lab", "12345678") != nil {
		t.Fatal("valid Wi-Fi settings rejected")
	}
	if validateWiFi("", "12345678") == nil || validateWiFi("lab", "short") == nil {
		t.Fatal("invalid Wi-Fi settings accepted")
	}
}
