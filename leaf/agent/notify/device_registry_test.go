package notify

import (
	"testing"
	"time"

	_ "github.com/danmestas/go-libfossil/db/driver/modernc"
)

func TestDeviceRegistryCRUD(t *testing.T) {
	r := createTestRepo(t)

	// Initially empty.
	devices, err := ListDevices(r)
	if err != nil {
		t.Fatal(err)
	}
	if len(devices) != 0 {
		t.Fatalf("expected 0 devices, got %d", len(devices))
	}

	// Add a device.
	dev := Device{
		Name:       "dan-iphone",
		EndpointID: "iroh-endpoint-abc123",
		PairedAt:   time.Now().UTC().Truncate(time.Second),
	}
	if err := AddDevice(r, dev); err != nil {
		t.Fatal(err)
	}

	devices, err = ListDevices(r)
	if err != nil {
		t.Fatal(err)
	}
	if len(devices) != 1 {
		t.Fatalf("expected 1 device, got %d", len(devices))
	}
	if devices[0].Name != "dan-iphone" {
		t.Errorf("name = %q", devices[0].Name)
	}

	// Duplicate name rejected.
	if err := AddDevice(r, dev); err == nil {
		t.Error("expected error for duplicate device name")
	}

	// Remove.
	if err := RemoveDevice(r, "dan-iphone"); err != nil {
		t.Fatal(err)
	}

	devices, err = ListDevices(r)
	if err != nil {
		t.Fatal(err)
	}
	if len(devices) != 0 {
		t.Fatalf("expected 0 devices after remove, got %d", len(devices))
	}

	// Remove non-existent = error.
	if err := RemoveDevice(r, "nonexistent"); err == nil {
		t.Error("expected error for removing non-existent device")
	}
}
