// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "board.h"

int wmain(int argc, wchar_t **argv)
{
    HANDLE device;
    DWORD bytes, error = ERROR_SUCCESS;
    PI5_BOARD_STATUS status;
    PI5_BOARD_SET command = {PI5_BOARD_VERSION, 0, 0, 0};
    unsigned long seconds = 0;
    wchar_t *end;
    if (argc == 5 && !wcscmp(argv[1], L"led")) {
        if (!wcscmp(argv[2], L"activity")) command.Pin = PI5_BOARD_ACTIVITY;
        else if (!wcscmp(argv[2], L"power")) command.Pin = PI5_BOARD_POWER_LED;
        else goto usage;
        if (!wcscmp(argv[3], L"on")) command.Asserted = 1;
        else if (wcscmp(argv[3], L"off")) goto usage;
        seconds = wcstoul(argv[4], &end, 10);
        if (!*argv[4] || *end || !seconds || seconds > 3600) goto usage;
    } else if (argc != 2 || wcscmp(argv[1], L"status")) goto usage;
    device = CreateFileW(L"\\\\.\\Pi5Board", GENERIC_READ | (seconds ? GENERIC_WRITE : 0),
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (device == INVALID_HANDLE_VALUE) { error = GetLastError(); goto failed; }
    if (seconds) {
        if (!DeviceIoControl(device, IOCTL_PI5_BOARD_SET, &command, sizeof(command), NULL, 0, &bytes, NULL))
            error = GetLastError();
        else {
            wprintf(L"%s LED %s for %lu seconds; closing restores its startup state.\n", argv[2], argv[3], seconds);
            Sleep(seconds * 1000);
        }
    } else if (!DeviceIoControl(device, IOCTL_PI5_BOARD_QUERY, NULL, 0, &status, sizeof(status), &bytes, NULL))
        error = GetLastError();
    else if (bytes != sizeof(status) || status.Version != PI5_BOARD_VERSION || status.Size != sizeof(status))
        error = ERROR_REVISION_MISMATCH;
    else printf("Configured=0x%02lx Leased=0x%02lx Levels=0x%02lx Readable=0x%02lx AlwaysOn=0x%02lx\n",
        status.Configured, status.Leased, status.Levels, status.Readable, status.AlwaysOn);
    CloseHandle(device);
failed:
    if (error) fprintf(stderr, "Pi5Board error %lu\n", error);
    return error ? 1 : 0;
usage:
    fputs("Pi5BoardTool status\nPi5BoardTool led activity|power on|off SECONDS (1..3600)\nRun as administrator.\n", stderr);
    return 2;
}
