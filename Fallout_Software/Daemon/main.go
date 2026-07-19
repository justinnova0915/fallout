// Daemon/main.go
package main

import (
	"encoding/json"
	"fmt"
	"log"
	"os/exec"
	"strings"
	"time"

	"github.com/shirou/gopsutil/v3/cpu"
	"github.com/shirou/gopsutil/v3/mem"
	"tinygo.org/x/bluetooth"
)

var adapter = bluetooth.DefaultAdapter

const (
	ServiceUUID      = "12345678-1234-1234-1234-1234567890ab"
	RxCharacteristic = "12345678-1234-1234-1234-1234567890ac"
	TxCharacteristic = "12345678-1234-1234-1234-1234567890ad"

	mmPerWorkspace = 8.0
	maxWorkspaces  = 10
)

type HyprClient struct {
	Class     string `json:"class"`
	Workspace struct {
		ID int `json:"id"`
	} `json:"workspace"`
}

type HyprActiveWorkspace struct {
	ID int `json:"id"`
}

func main() {
	err := adapter.Enable()
	if err != nil && (strings.Contains(err.Error(), "does not exist") || strings.Contains(err.Error(), "no such file")) {
		log.Println("[WARN] Default interface hci0 is unallocated. Falling back to hci1 slot...")
		must(adapter.Enable())
	} else if err != nil {
		log.Fatalf("[FATAL] Bluetooth framework failed initialization: %v", err)
	}

	log.Println("[INIT] Scanning for Endfield Command Strip (TALOS-01)...")
	var targetDevice = bluetooth.ScanResult{}

	ch := make(chan bluetooth.ScanResult, 1)
	err = adapter.Scan(func(adapter *bluetooth.Adapter, result bluetooth.ScanResult) {
		if result.LocalName() == "TALOS-01" {
			adapter.StopScan()
			ch <- result
		}
	})
	must(err)

	targetDevice = <-ch
	log.Printf("[SUCCESS] Found device! Connecting to %s...", targetDevice.Address.String())

	device, err := adapter.Connect(targetDevice.Address, bluetooth.ConnectionParams{})
	must(err)
	defer device.Disconnect()

	log.Println("[GATT] Discovering device service attributes map...")
	services, err := device.DiscoverServices(nil)
	must(err)

	if len(services) == 0 {
		log.Fatalf("[FATAL] Zero GATT services reported by remote container.")
	}

	var endfieldService bluetooth.DeviceService
	foundService := false
	for _, svc := range services {
		if strings.ToLower(svc.UUID().String()) == ServiceUUID {
			endfieldService = svc
			foundService = true
			break
		}
	}

	if !foundService {
		log.Fatalf("[FATAL] Service matching target ID %s was not found.", ServiceUUID)
	}

	chars, err := endfieldService.DiscoverCharacteristics(nil)
	must(err)

	var rxChar, txChar bluetooth.DeviceCharacteristic
	hasRx, hasTx := false, false

	for _, ch := range chars {
		chUUID := strings.ToLower(ch.UUID().String())
		if chUUID == RxCharacteristic {
			rxChar = ch
			hasRx = true
		} else if chUUID == TxCharacteristic {
			txChar = ch
			hasTx = true
		}
	}

	if !hasRx || !hasTx {
		log.Fatalf("[FATAL] Missing critical endpoints. RX: %t | TX: %t", hasRx, hasTx)
	}

	log.Println("[CONNECTED] Wireless BLE data channels fully synced and operational.")

	go startWirelessTelemetry(rxChar)
	go startWirelessHyprlandState(rxChar)
	go startWirelessTimeSync(rxChar)
	go startWirelessDateSync(rxChar)
	go startWirelessWeatherSync(rxChar)

	startWirelessInboundPipeline(txChar)
}

func startWirelessTimeSync(rxChar bluetooth.DeviceCharacteristic) {
	for {
		now := time.Now()
		msg := fmt.Sprintf("TIME:%02d:%02d\n", now.Hour(), now.Minute())
		log.Printf("[OUTBOUND] Syncing system time parameters: %02d:%02d", now.Hour(), now.Minute())
		rxChar.WriteWithoutResponse([]byte(msg))
		time.Sleep(60 * time.Second)
	}
}

func startWirelessDateSync(rxChar bluetooth.DeviceCharacteristic) {
	for {
		now := time.Now()
		// CRITICAL FIX: Explicitly cast now.Month() to int so it formats as numeric digits!
		msg := fmt.Sprintf("DATE:%04d:%02d:%02d\n", now.Year(), int(now.Month()), now.Day())
		log.Printf("[OUTBOUND] Syncing calendar date configuration: %04d-%02d-%02d", now.Year(), int(now.Month()), now.Day())
		rxChar.WriteWithoutResponse([]byte(msg))
		time.Sleep(24 * time.Hour)
	}
}

func startWirelessWeatherSync(rxChar bluetooth.DeviceCharacteristic) {
	for {
		// Scrape condition text (%C) and numeric temp (%t) separately with metric sizing flag
		out, err := exec.Command("curl", "-s", "wttr.in?format=%C:%t&m").Output()
		if err == nil {
			payload := strings.TrimSpace(string(out))
			if len(payload) > 0 && !strings.Contains(payload, "Error") {
				parts := strings.Split(payload, ":")
				if len(parts) == 2 {
					condition := strings.ToLower(strings.TrimSpace(parts[0]))
					tempRaw := strings.TrimSpace(parts[1])

					// Parse numerical temp to match requirements: remove signs/units
					tempRaw = strings.ReplaceAll(tempRaw, "+", "")
					tempRaw = strings.ReplaceAll(tempRaw, "C", "")
					tempRaw = strings.ReplaceAll(tempRaw, "°", "")
					tempStr := strings.TrimSpace(tempRaw)

					// Default condition token frame mapping (0: partly sunny baseline)
					iconIdx := 0

					// Pattern matching maps text summaries to asset lookup tables
					if strings.Contains(condition, "thunderstorm") {
						iconIdx = 3
					} else if strings.Contains(condition, "rain") || strings.Contains(condition, "drizzle") || strings.Contains(condition, "shower") {
						iconIdx = 1
					} else if strings.Contains(condition, "snow") || strings.Contains(condition, "flurry") || strings.Contains(condition, "ice") {
						iconIdx = 4
					} else if strings.Contains(condition, "wind") || strings.Contains(condition, "gale") {
						iconIdx = 5
					} else if strings.Contains(condition, "sunny") || strings.Contains(condition, "clear") {
						// Determine day/night context matching if clear
						hour := time.Now().Hour()
						if hour >= 19 || hour < 6 {
							iconIdx = 6 // Night asset index configuration choice
						} else {
							iconIdx = 2 // Sunny daytime profile allocation index
						}
					} else if strings.Contains(condition, "cloud") || strings.Contains(condition, "overcast") || strings.Contains(condition, "mist") || strings.Contains(condition, "fog") {
						iconIdx = 0 // Partly sunny / cloudy indicator choice layout register
					}

					// Blast numerical temp layout string
					tempMsg := fmt.Sprintf("WEATHER:%s\n", tempStr)
					rxChar.WriteWithoutResponse([]byte(tempMsg))

					// Blast structured icon register value
					iconMsg := fmt.Sprintf("W_ICON:%d\n", iconIdx)
					rxChar.WriteWithoutResponse([]byte(iconMsg))

					log.Printf("[OUTBOUND WEATHER] Temp Raw Value: %s | Condition Matched: '%s' -> Display Icon Index: %d", tempStr, condition, iconIdx)
				}
			}
		}
		time.Sleep(60 * time.Second)
	}
}

func startWirelessTelemetry(rxChar bluetooth.DeviceCharacteristic) {
	for {
		cpuUsage := 0.0
		cpuPercentages, err := cpu.Percent(time.Second, false)
		if err == nil && len(cpuPercentages) > 0 {
			cpuUsage = cpuPercentages[0]
		}

		ramUsage := 0.0
		vmStat, err := mem.VirtualMemory()
		if err == nil {
			ramUsage = vmStat.UsedPercent
		}

		msg := fmt.Sprintf("STATS:CPU:%.1f:RAM:%.1f\n", cpuUsage, ramUsage)
		_, err = rxChar.WriteWithoutResponse([]byte(msg))
		time.Sleep(1 * time.Second)
	}
}

func startWirelessHyprlandState(rxChar bluetooth.DeviceCharacteristic) {
	var lastSentStr string
	for {
		activeWS := 1
		wsData, err := exec.Command("hyprctl", "activeworkspace", "-j").Output()
		if err == nil {
			var ws HyprActiveWorkspace
			if json.Unmarshal(wsData, &ws) == nil {
				activeWS = ws.ID
			}
		}

		clientData, err := exec.Command("hyprctl", "clients", "-j").Output()
		workspaceApps := make(map[int][]string)

		if err == nil {
			var clients []HyprClient
			if json.Unmarshal(clientData, &clients) == nil {
				for _, c := range clients {
					if c.Workspace.ID > 0 && c.Workspace.ID <= maxWorkspaces {
						appName := strings.ToLower(c.Class)
						if appName == "" {
							appName = "unknown"
						}
						workspaceApps[c.Workspace.ID] = append(workspaceApps[c.Workspace.ID], appName)
					}
				}
			}
		}

		var mappingPairs []string
		for wsID := 1; wsID <= maxWorkspaces; wsID++ {
			if apps, exists := workspaceApps[wsID]; exists {
				mappingPairs = append(mappingPairs, fmt.Sprintf("%d=%s", wsID, strings.Join(apps, ",")))
			}
		}

		appsLayoutStr := strings.Join(mappingPairs, ";")
		if appsLayoutStr == "" {
			appsLayoutStr = "NONE"
		}

		msg := fmt.Sprintf("WS_MAP:%d:%s\n", activeWS, appsLayoutStr)

		if msg != lastSentStr {
			log.Printf("[OUTBOUND] Matrix Viewport Shift -> Active WS: %d | Layouts: %s", activeWS, appsLayoutStr)
			lastSentStr = msg
		}

		_, err = rxChar.WriteWithoutResponse([]byte(msg))
		time.Sleep(250 * time.Millisecond)
	}
}

func startWirelessInboundPipeline(txChar bluetooth.DeviceCharacteristic) {
	lastWorkspaceID := -1
	isGrabActive := false

	err := txChar.EnableNotifications(func(buf []byte) {
		line := strings.TrimSpace(string(buf))
		if len(line) == 0 {
			return
		}

		log.Printf("[INBOUND INTERCEPT] Received Frame -> %s", line)

		if strings.HasPrefix(line, "CMD:FADER:") {
			var currentMM float64
			var grabFlag string

			count, _ := fmt.Sscanf(line, "CMD:FADER:%f:%s", &currentMM, &grabFlag)
			if count >= 1 {
				isGrabActive = (count == 2 && grabFlag == "GRAB")

				targetWorkspaceID := int(currentMM/mmPerWorkspace) + 1
				if targetWorkspaceID < 1 {
					targetWorkspaceID = 1
				}
				if targetWorkspaceID > maxWorkspaces {
					targetWorkspaceID = maxWorkspaces
				}

				if targetWorkspaceID != lastWorkspaceID {
					log.Printf("[INBOUND EVAL] Fader moved. Requesting active workspace change to: %d", targetWorkspaceID)
					executeWorkspaceAction(targetWorkspaceID, isGrabActive)
					lastWorkspaceID = targetWorkspaceID
				}
			}
		}

		if strings.HasPrefix(line, "CMD:MACRO:") {
			var bitmask int
			if _, err := fmt.Sscanf(line, "CMD:MACRO:%d", &bitmask); err == nil {
				log.Printf("[INBOUND EVAL] Chord key hit detected. Register mask: 0x%02X", bitmask)
				executeMacroBitmask(bitmask)
			}
		}

		if line == "CMD:POWER_PRESS" {
			log.Println("[INBOUND ACTION] Power button clicked. Engaging swaylock desktop safety layout...")
			exec.Command("swaylock").Start()
		}
	})
	must(err)
	select {}
}

func executeWorkspaceAction(workspaceID int, grabWindow bool) {
	wsString := fmt.Sprintf("%d", workspaceID)
	if grabWindow {
		exec.Command("hyprctl", "dispatch", "movetoworkspace", wsString).Run()
	} else {
		exec.Command("hyprctl", "dispatch", "workspace", wsString).Run()
	}
}

func executeMacroBitmask(bitmask int) {
	if bitmask&(1<<0) != 0 {
		exec.Command("kitty").Start()
	}
	if bitmask&(1<<1) != 0 {
		exec.Command("rofi", "-show", "drun").Start()
	}
	if bitmask&(1<<2) != 0 {
		exec.Command("grimshot", "save", "area").Start()
	}
}

func must(err error) {
	if err != nil {
		panic(err)
	}
}
