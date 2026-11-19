// ble_temp.cpp - Windows BLE client for the nRF52840-DK "nRF-Temp" board.
//
// Scans for "nRF-Temp", connects, subscribes to the standard Temperature
// characteristic (0x2A6E in Environmental Sensing 0x181A) and prints each
// notification in degrees Celsius.
//
// Build (Developer PowerShell for VS 2022):
//     cl /std:c++20 /EHsc /W4 /permissive- ble_temp.cpp
// Run:
//     .\ble_temp.exe
//
// Licence: MIT

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Storage.Streams.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <future>
#include <iostream>
#include <string>

#pragma comment(lib, "windowsapp")   // so you don't need it on the cl command line

// Short aliases instead of "using namespace" (no namespace traps here)
namespace wf      = winrt::Windows::Foundation;
namespace bt      = winrt::Windows::Devices::Bluetooth;
namespace adv     = winrt::Windows::Devices::Bluetooth::Advertisement;
namespace gatt    = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
namespace streams = winrt::Windows::Storage::Streams;

constexpr char     kDeviceName[] = "nRF-Temp";
constexpr uint32_t kEssUuid      = 0x181A;   // Environmental Sensing service
constexpr uint32_t kTempUuid     = 0x2A6E;   // Temperature characteristic
constexpr auto     kScanTimeout  = std::chrono::seconds(30);

// 0x0000AABBCCDDEEFF -> "AA:BB:CC:DD:EE:FF"
static std::string mac_to_string(uint64_t a)
{
    char s[18];
    std::snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X",
                  static_cast<unsigned>((a >> 40) & 0xFF), static_cast<unsigned>((a >> 32) & 0xFF),
                  static_cast<unsigned>((a >> 24) & 0xFF), static_cast<unsigned>((a >> 16) & 0xFF),
                  static_cast<unsigned>((a >> 8) & 0xFF),  static_cast<unsigned>(a & 0xFF));
    return s;
}

// The value is a signed 16-bit integer, little-endian, in hundredths of a degree
static void print_temperature(streams::IBuffer const& buffer)
{
    auto reader = streams::DataReader::FromBuffer(buffer);
    reader.ByteOrder(streams::ByteOrder::LittleEndian);
    if (reader.UnconsumedBufferLength() < 2) {
        std::printf("(short packet: %u bytes)\n", reader.UnconsumedBufferLength());
        return;
    }
    const int16_t centi = reader.ReadInt16();
    std::printf("Temperature: %.2f C\n", centi / 100.0);
}

int main()
{
    winrt::init_apartment();   // multi-threaded apartment: lets main() block on .get()

    try {
        // ---- 1. Scan (Observer) until we hear "nRF-Temp" -------------------
        std::printf("Scanning for %s... (press RESET on the board if it doesn't show up)\n", kDeviceName);

        adv::BluetoothLEAdvertisementWatcher watcher;
        watcher.ScanningMode(adv::BluetoothLEScanningMode::Active);   // name is in the scan response

        std::promise<uint64_t> found;
        std::atomic<bool> already_found{false};

        // Runs on a Windows thread for every packet heard: only store and signal, never block here
        auto scan_token = watcher.Received(
            [&](adv::BluetoothLEAdvertisementWatcher const&,
                adv::BluetoothLEAdvertisementReceivedEventArgs const& args) {
                if (winrt::to_string(args.Advertisement().LocalName()) == kDeviceName &&
                    !already_found.exchange(true)) {
                    found.set_value(args.BluetoothAddress());
                }
            });
        watcher.Start();

        auto future = found.get_future();
        if (future.wait_for(kScanTimeout) != std::future_status::ready) {
            watcher.Stop();
            std::fprintf(stderr, "%s not found. Is it advertising, and is the phone disconnected?\n", kDeviceName);
            return 1;
        }
        const uint64_t address = future.get();
        watcher.Stop();
        watcher.Received(scan_token);   // unregister the handler
        std::printf("Found %s at %s\n", kDeviceName, mac_to_string(address).c_str());

        // ---- 2. Connect (Central) ------------------------------------------
        auto device = bt::BluetoothLEDevice::FromBluetoothAddressAsync(address).get();
        if (!device) {
            std::fprintf(stderr, "Could not open the device.\n");
            return 1;
        }
        device.ConnectionStatusChanged(
            [](bt::BluetoothLEDevice const& d, wf::IInspectable const&) {
                std::printf(d.ConnectionStatus() == bt::BluetoothConnectionStatus::Connected
                                ? "[connected]\n" : "[disconnected]\n");
            });

        // ---- 3. Find the service, then the characteristic ------------------
        // Uncached = ask the board, not Windows' memory of an older firmware's GATT table
        auto services = device.GetGattServicesForUuidAsync(
                            bt::BluetoothUuidHelper::FromShortId(kEssUuid),
                            bt::BluetoothCacheMode::Uncached).get();
        if (services.Status() != gatt::GattCommunicationStatus::Success || services.Services().Size() == 0) {
            std::fprintf(stderr, "Environmental Sensing service not found.\n");
            return 1;
        }
        auto service = services.Services().GetAt(0);

        auto chars = service.GetCharacteristicsForUuidAsync(
                         bt::BluetoothUuidHelper::FromShortId(kTempUuid),
                         bt::BluetoothCacheMode::Uncached).get();
        if (chars.Status() != gatt::GattCommunicationStatus::Success || chars.Characteristics().Size() == 0) {
            std::fprintf(stderr, "Temperature characteristic not found.\n");
            return 1;
        }
        auto temperature = chars.Characteristics().GetAt(0);

        auto descriptors = temperature.GetDescriptorsForUuidAsync(
                       bt::BluetoothUuidHelper::FromShortId(0x2901),   // User Description
                       bt::BluetoothCacheMode::Uncached).get();
if (descriptors.Status() == gatt::GattCommunicationStatus::Success && descriptors.Descriptors().Size() > 0) {
    auto label = descriptors.Descriptors().GetAt(0).ReadValueAsync(bt::BluetoothCacheMode::Uncached).get();
    if (label.Status() == gatt::GattCommunicationStatus::Success) {
        auto reader = streams::DataReader::FromBuffer(label.Value());
        reader.UnicodeEncoding(streams::UnicodeEncoding::Utf8);
        std::printf("Sensor says: %s\n",
                    winrt::to_string(reader.ReadString(reader.UnconsumedBufferLength())).c_str());
    }
} else {
    std::printf("No label found - is this the right board/firmware?\n");
}

        // ---- 4. Read once, so there's a number on screen straight away -----
        auto first = temperature.ReadValueAsync(bt::BluetoothCacheMode::Uncached).get();
        if (first.Status() == gatt::GattCommunicationStatus::Success) {
            print_temperature(first.Value());
        }

        // ---- 5. Subscribe: handler first, then flip the CCC switch ---------
        auto notify_token = temperature.ValueChanged(
            [](gatt::GattCharacteristic const&, gatt::GattValueChangedEventArgs const& args) {
                print_temperature(args.CharacteristicValue());
            });

        auto status = temperature.WriteClientCharacteristicConfigurationDescriptorAsync(
                          gatt::GattClientCharacteristicConfigurationDescriptorValue::Notify).get();
        if (status != gatt::GattCommunicationStatus::Success) {
            std::fprintf(stderr, "Couldn't enable notifications.\n");
            return 1;
        }
        std::printf("Subscribed. Press Enter to quit.\n");

        std::cin.get();

        // ---- 6. Tidy up ----------------------------------------------------
        temperature.ValueChanged(notify_token);
        temperature.WriteClientCharacteristicConfigurationDescriptorAsync(
            gatt::GattClientCharacteristicConfigurationDescriptorValue::None).get();
        service.Close();
        device.Close();
    }
    catch (winrt::hresult_error const& e) {
        std::fprintf(stderr, "Windows error 0x%08X: %s\n",
                     static_cast<unsigned>(e.code().value), winrt::to_string(e.message()).c_str());
        return 1;
    }
    return 0;
}