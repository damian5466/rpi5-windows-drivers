// SPDX-License-Identifier: BSD-2-Clause-Patent
// A desktop client of the standard Windows GPIO API (Resource Hub Proxy).
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.Gpio.h>
#include <atomic>
#include <cstdio>
#include <string>

using namespace winrt;
using namespace Windows::Devices::Gpio;

static int Number(const wchar_t *text, int maximum)
{
    wchar_t *end = nullptr;
    long value = wcstol(text, &end, 10);
    if (!*text || *end || value < 0 || value > maximum) throw hresult_invalid_argument();
    return static_cast<int>(value);
}
int wmain(int argc, wchar_t **argv)
{
    try {
        init_apartment(apartment_type::multi_threaded);
        if (argc < 2) {
            std::puts("Pi5GpioTool list | read GPIO [none|up|down] | write GPIO 0|1 [hold-ms] | watch GPIO [seconds]\nGPIO numbers, not physical header pin numbers. Close restores the pin.");
            return 2;
        }
        auto controller = GpioController::GetDefault();
        if (!controller) { std::fputs("No Windows GPIO controller found. Install RP1 GPIO and matching firmware.\n", stderr); return 1; }
        std::wstring command(argv[1]);
        if (command == L"list" && argc == 2) {
            std::printf("Windows GPIO controller: %d pins; Pi 5 header GPIO2..27; GPIO0/1 reserved.\n", controller.PinCount());
            return 0;
        }
        if (argc < 3) throw hresult_invalid_argument();
        auto number = Number(argv[2], 27);
        if (number < 2) throw hresult_invalid_argument();
        auto pin = controller.OpenPin(number);
        if (command == L"read" && argc <= 4) {
            GpioPinDriveMode mode = GpioPinDriveMode::Input;
            if (argc == 4) {
                std::wstring pull(argv[3]);
                if (pull == L"up") mode = GpioPinDriveMode::InputPullUp;
                else if (pull == L"down") mode = GpioPinDriveMode::InputPullDown;
                else if (pull != L"none") throw hresult_invalid_argument();
            }
            pin.SetDriveMode(mode);
            Sleep(10);
            std::printf("GPIO%d=%d\n", number, pin.Read() == GpioPinValue::High);
        } else if (command == L"write" && (argc == 4 || argc == 5)) {
            auto value = Number(argv[3], 1);
            auto duration = argc == 5 ? Number(argv[4], 3600000) : 1000;
            pin.Write(value ? GpioPinValue::High : GpioPinValue::Low);
            pin.SetDriveMode(GpioPinDriveMode::Output);
            std::printf("GPIO%d=%d for %d ms\n", number, value, duration);
            Sleep(static_cast<DWORD>(duration));
        } else if (command == L"watch" && argc <= 4) {
            auto duration = argc == 4 ? Number(argv[3], 3600) : 10;
            pin.SetDriveMode(GpioPinDriveMode::InputPullDown);
            pin.DebounceTimeout(std::chrono::milliseconds(1));
            auto token = pin.ValueChanged([number](auto const &, GpioPinValueChangedEventArgs const &event) {
                std::printf("GPIO%d %s\n", number, event.Edge() == GpioPinEdge::RisingEdge ? "rising" : "falling");
                std::fflush(stdout);
            });
            Sleep(static_cast<DWORD>(duration * 1000));
            pin.ValueChanged(token);
        } else throw hresult_invalid_argument();
        pin.Close();
        return 0;
    } catch (hresult_error const &error) {
        std::fwprintf(stderr, L"GPIO error 0x%08X: %ls\n", static_cast<unsigned>(error.code().value), error.message().c_str());
        return 1;
    }
}
