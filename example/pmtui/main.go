package main

import (
	"flag"
	"fmt"
	"os"
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "pmtui:", err)
		os.Exit(1)
	}
}

func run() error {
	host := flag.String("host", "192.168.4.1", "PowerMate host, optionally including http:// or https://")
	id := flag.String("id", "admin", "PowerMate login username")
	uart := flag.Bool("uart", false, "Open the UART terminal after login")
	flag.Parse()

	return newTUI(*host, *id, *uart).Run()
}
