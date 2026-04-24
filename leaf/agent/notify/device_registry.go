package notify

import (
	"encoding/json"
	"fmt"
	"time"

	libfossil "github.com/danmestas/libfossil"
)

const devicesFilePath = "_notify/devices.json"

// Device is a paired device entry in the registry.
type Device struct {
	Name       string    `json:"name"`
	EndpointID string    `json:"endpoint_id"`
	PairedAt   time.Time `json:"paired_at"`
}

// DeviceRegistry is the JSON structure stored in the notify repo.
type DeviceRegistry struct {
	Devices []Device `json:"devices"`
}

// ListDevices reads the device registry from the notify repo.
// Returns an empty slice (not error) if the file doesn't exist yet.
func ListDevices(r *libfossil.Repo) ([]Device, error) {
	data, err := readFileContent(r, devicesFilePath)
	if err != nil {
		// If file doesn't exist, return empty.
		return nil, nil
	}
	var reg DeviceRegistry
	if err := json.Unmarshal(data, &reg); err != nil {
		return nil, fmt.Errorf("unmarshal device registry: %w", err)
	}
	return reg.Devices, nil
}

// AddDevice adds a device to the registry. Rejects duplicate names.
func AddDevice(r *libfossil.Repo, dev Device) error {
	devices, err := ListDevices(r)
	if err != nil {
		return err
	}
	for _, d := range devices {
		if d.Name == dev.Name {
			return fmt.Errorf("device %q already exists", dev.Name)
		}
	}
	devices = append(devices, dev)
	return commitDevices(r, devices)
}

// RemoveDevice removes a device by name. Returns error if not found.
func RemoveDevice(r *libfossil.Repo, name string) error {
	devices, err := ListDevices(r)
	if err != nil {
		return err
	}
	found := false
	filtered := make([]Device, 0, len(devices))
	for _, d := range devices {
		if d.Name == name {
			found = true
			continue
		}
		filtered = append(filtered, d)
	}
	if !found {
		return fmt.Errorf("device %q not found", name)
	}
	return commitDevices(r, filtered)
}

// commitDevices serializes the device list and commits it to the notify repo.
func commitDevices(r *libfossil.Repo, devices []Device) error {
	reg := DeviceRegistry{Devices: devices}
	data, err := json.MarshalIndent(reg, "", "  ")
	if err != nil {
		return fmt.Errorf("marshal device registry: %w", err)
	}
	_, _, err = r.Commit(libfossil.CommitOpts{
		Files: []libfossil.FileToCommit{
			{Name: devicesFilePath, Content: data},
		},
		Comment: "notify: update device registry",
		User:    "notify",
	})
	if err != nil {
		return fmt.Errorf("commit device registry: %w", err)
	}
	return nil
}
