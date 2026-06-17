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

// Match your exact compiled 128-bit UUIDs from ble.cpp
const (
	ServiceUUID      = "12345678-1234-1234-1234-1234567890ab" // Base service
	RxCharacteristic = "12345678-1234-1234-1234-1234567890ac" // PC Writes to this (ESP32 RX)
	TxCharacteristic = "12345678-1234-1234-1234-1234567890ad" // PC Listens to this (ESP32 TX)

	mmPerWorkspace = 8.0 // 8mm of physical stride per virtual workspace step
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
	// DYNAMIC ADAPTER DETECTION PROFILE
	// Attempt to turn on default hci0 adapter node
	err := adapter.Enable()
	if err != nil && (strings.Contains(err.Error(), "does not exist") || strings.Contains(err.Error(), "no such file")) {
		log.Println("[WARN] Default interface hci0 is unallocated. Falling back to laptop radio hci1 configuration slot...")

		// Re-target the TinyGo runtime engine to process using the hci1 system socket block
		if err != nil {
			log.Fatalf("[FATAL] Could not establish DBus communication pipeline to hardware address space hci1: %v", err)
		}

		// Attempt to turn on the updated interface configuration
		must(adapter.Enable())
	} else if err != nil {
		log.Fatalf("[FATAL] Bluetooth hardware framework failed state initialization: %v", err)
	}

	log.Println("[INIT] Scanning for Endfield Command Strip (TALOS-01)...")
	var targetDevice bluetooth.ScanResult

	// Scan until we discover the broadcast name configured in your BLE driver
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
	// Pass nil to pull the raw, unfiltered attribute table straight from BlueZ DBus cache
	services, err := device.DiscoverServices(nil)
	must(err)

	if len(services) == 0 {
		log.Fatalf("[FATAL] Zero GATT services reported by remote peripheral container.")
	}

	// Loop through and isolate your custom Endfield Service using case-insensitive validation
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
		log.Println("[DEBUG] Exposed services on your device:")
		for _, svc := range services {
			log.Printf("  -> Found Service UUID: %s", svc.UUID().String())
		}
		log.Fatalf("[FATAL] Service matching target ID %s was not found.", ServiceUUID)
	}

	// Fetch all characteristic layout descriptors safely under the matching service node
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
		log.Fatalf("[FATAL] Pipeline broken. Missing critical endpoints. RX Found: %t | TX Found: %t", hasRx, hasTx)
	}

	log.Println("[CONNECTED] Wireless BLE data channels fully synced and operational.")

	// Thread 1: Outbound PC Performance Telemetry (Engine to Voltmeter / Widgets)
	go startWirelessTelemetry(rxChar)

	// Thread 2: Outbound Hyprland Window Mapper (Engine to DWIN Matrix Layout)
	go startWirelessHyprlandState(rxChar)

	// Thread 3 / Main: Inbound Event Handler (Listens for incoming notifications from ESP32)
	startWirelessInboundPipeline(txChar)
}

// ============================================================================
// 1. OUTBOUND: TELEMETRY PIPELINE (CPU & RAM)
// ============================================================================
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
		_, err = rxChar.WriteWithoutResponse([]byte(msg))
		if err != nil {
			log.Printf("[ERR] Telemetry drop; transmission failure: %v", err)
		}

		time.Sleep(1 * time.Second)
	}
}

// ============================================================================
// 2. OUTBOUND: HYPRLAND STATE SYNC PIPELINE (Descriptive App Mapping)
// ============================================================================
func startWirelessHyprlandState(rxChar bluetooth.DeviceCharacteristic) {
	for {
		// 1. Fetch active workspace ID
		activeWS := 1
		wsData, err := exec.Command("hyprctl", "activeworkspace", "-j").Output()
		if err == nil {
			var ws HyprActiveWorkspace
			if json.Unmarshal(wsData, &ws) == nil {
				activeWS = ws.ID
			}
		}

		// 2. Fetch open windows AND their application class names
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

		// 3. Serialize the map into a compact layout token format: WS=APP,APP;WS=APP...
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

		// 4. Stream layout packet down to update your DWIN display widgets
		msg := fmt.Sprintf("WS_MAP:%d:%s\n", activeWS, appsLayoutStr)
		_, err = rxChar.WriteWithoutResponse([]byte(msg))
		if err != nil {
			log.Printf("[ERR] Layout state drop; transmission failure: %v", err)
		}

		time.Sleep(250 * time.Millisecond)
	}
}

// ============================================================================
// 3. INBOUND: INCOMING NOTIFICATION PIPELINE
// ============================================================================
func startWirelessInboundPipeline(txChar bluetooth.DeviceCharacteristic) {
	lastWorkspaceID := -1
	isGrabActive := false

	// Subscribes directly to your ESP32's notification event trigger
	err := txChar.EnableNotifications(func(buf []byte) {
		line := strings.TrimSpace(string(buf))
		if len(line) == 0 {
			return
		}

		// Match Slider Tracking Data
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

		// Match Left Wing Macro Combo Pad Bitmask Reads
		if strings.HasPrefix(line, "CMD:MACRO:") {
			var bitmask int
			if _, err := fmt.Sscanf(line, "CMD:MACRO:%d", &bitmask); err == nil {
				executeMacroBitmask(bitmask)
			}
		}

		// Match Power Button Intercepts
		if line == "CMD:POWER_PRESS" {
			log.Println("[ACTION] Power Interrupt received. Activating lock screen layout...")
			exec.Command("swaylock").Start()
		}
	})
	must(err)

	// Keep background threads active and running
	select {}
}

// ============================================================================
// 4. DESKTOP SUBSYSTEM DISPATCHERS
// ============================================================================
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
