// main.go
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

// Match your exact compiled 128-bit UUIDs from ble.cpp
const (
	ServiceUUID      = "12345678-1234-1234-1234-1234567890ab" // Base service
	RxCharacteristic = "12345678-1234-1234-1234-1234567890ac" // PC Writes to this (ESP32 RX)
	TxCharacteristic = "12345678-1234-1234-1234-1234567890ad" // PC Listens to this (ESP32 TX)

	mmPerWorkspace = 8.0 // 8mm of physical stride per virtual workspace window step
	maxWorkspaces  = 10  // Total workspace limits
)

// Hyprland JSON IPC Structural Definitions
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
	// Enable Bluetooth
	must(adapter.Enable())

	log.Println("Searching...")
	var targetDevice bluetooth.ScanResult

	// Scanning
	ch := make(chan bluetooth.ScanResult, 1)
	err := adapter.Scan(func(adapter *bluetooth.Adapter, result bluetooth.ScanResult) {
		if result.LocalName() == "TALOS-01" {
			adapter.StopScan()
			ch <- result
		}
	})
	must(err)

	targetDevice = <-ch
	log.Printf("Connected to %s...", targetDevice.Address.String())

	device, err := adapter.Connect(targetDevice.Address, bluetooth.ConnectionParams{})
	must(err)
	defer device.Disconnect()

	// Pairing
	parsedSvcUUID, err := bluetooth.ParseUUID(ServiceUUID)
	must(err)
	services, err := device.DiscoverServices([]bluetooth.UUID{parsedSvcUUID})
	must(err)
	if len(services) == 0 {
		log.Fatalf("[FATAL] Could not find GATT service on device.")
	}

	// Setting up Rx and Tx channels
	parsedRxUUID, _ := bluetooth.ParseUUID(RxCharacteristic)
	parsedTxUUID, _ := bluetooth.ParseUUID(TxCharacteristic)
	chars, err := services[0].DiscoverCharacteristics([]bluetooth.UUID{parsedRxUUID, parsedTxUUID})
	must(err)

	var rxChar, txChar bluetooth.DeviceCharacteristic
	for _, ch := range chars {
		if ch.UUID().String() == RxCharacteristic {
			rxChar = ch
		} else if ch.UUID().String() == TxCharacteristic {
			txChar = ch
		}
	}

	log.Println("Connected to channels")

	// Start threads
	go startWirelessTelemetry(rxChar)

	go startWirelessHyprlandState(rxChar)

	startWirelessInboundPipeline(txChar)
}

// Sends Hardware data over
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

		// Pack stats into a string format matching your main.cpp sscanf tracker
		msg := fmt.Sprintf("STATS:CPU:%.1f:RAM:%.1f\n", cpuUsage, ramUsage)
		rxChar.WriteWithoutResponse([]byte(msg))

		time.Sleep(1 * time.Second)
	}
}

// Send workspace information
func startWirelessHyprlandState(rxChar bluetooth.DeviceCharacteristic) {
	for {

		// get active workspace
		activeWS := 1
		wsData, err := exec.Command("hyprctl", "activeworkspace", "-j").Output()
		if err == nil {
			var ws HyprActiveWorkspace
			if json.Unmarshal(wsData, &ws) == nil {
				activeWS = ws.ID
			}
		}

		// Gets windows and the Apps open in them
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

		// Making the bluetooth packets
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

		// Send them over
		// Format sent: WS_MAP:3:1=kitty;3=discord,kitty;5=firefox\n
		msg := fmt.Sprintf("WS_MAP:%d:%s\n", activeWS, appsLayoutStr)
		rxChar.WriteWithoutResponse([]byte(msg))

		time.Sleep(250 * time.Millisecond)
	}
}

// Reciving data from esp32
func startWirelessInboundPipeline(txChar bluetooth.DeviceCharacteristic) {
	lastWorkspaceID := -1
	isGrabActive := false

	err := txChar.EnableNotifications(func(buf []byte) {
		line := strings.TrimSpace(string(buf))
		if len(line) == 0 {
			return
		}

		// Parcing the data
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
					executeWorkspaceAction(targetWorkspaceID, isGrabActive)
					lastWorkspaceID = targetWorkspaceID
				}
			}
		}

		// Parcing Macro pad key presses
		if strings.HasPrefix(line, "CMD:MACRO:") {
			var bitmask int
			if _, err := fmt.Sscanf(line, "CMD:MACRO:%d", &bitmask); err == nil {
				executeMacroBitmask(bitmask)
			}
		}

		// Parce power button
		if line == "CMD:POWER_PRESS" {
			log.Println("[ACTION] Power Interrupt received. Activating lock screen layout...")
			exec.Command("swaylock").Start()
		}
	})
	must(err)

	// Keep background threads active and running
	select {}
}

// Hyprland helper function
func executeWorkspaceAction(workspaceID int, grabWindow bool) {
	wsString := fmt.Sprintf("%d", workspaceID)

	if grabWindow {
		log.Printf("[HYPRLAND] DRAG WINDOW: Displacing focus window stack container into Workspace %d", workspaceID)
		exec.Command("hyprctl", "dispatch", "movetoworkspace", wsString).Run()
	} else {
		log.Printf("[HYPRLAND] NAVIGATE: Dispatches current system view to Workspace %d", workspaceID)
		exec.Command("hyprctl", "dispatch", "workspace", wsString).Run()
	}
}

// Keybind
func executeMacroBitmask(bitmask int) {
	log.Printf("[MACROPAD] Processing key state mapping configuration chord: 0x%04X", bitmask)

	if bitmask&(1<<0) != 0 { // Key 1
		exec.Command("kitty").Start()
	}
	if bitmask&(1<<1) != 0 { // Key 2
		exec.Command("rofi", "-show", "drun").Start()
	}
	if bitmask&(1<<2) != 0 { // Key 3
		exec.Command("grimshot", "save", "area").Start()
	}
}

// Simple panic handler for clean initialization checks
func must(err error) {
	if err != nil {
		panic(err)
	}
}
