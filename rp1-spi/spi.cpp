// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Spi.h>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>
#include <stdexcept>
using namespace winrt;
using namespace Windows::Devices::Enumeration;
using namespace Windows::Devices::Spi;
static unsigned number(const wchar_t* text,unsigned max) {
    wchar_t* end=nullptr;unsigned long n=wcstoul(text,&end,0);
    if(!*text||*end||*text==L'-'||n>max)throw std::runtime_error("Invalid number");
    return static_cast<unsigned>(n);
}
int wmain(int argc,wchar_t** argv) {
    try {
        init_apartment(apartment_type::multi_threaded);
        if(argc<2){
            puts("Pi5SpiTool list\nPi5SpiTool write BUS CS HZ MODE BYTE...\nPi5SpiTool read BUS CS HZ MODE COUNT\nPi5SpiTool transfer BUS CS HZ MODE BYTE...\nNumbers accept decimal or 0x-prefixed hexadecimal. CS: 0/1; HZ: 100000..4000000; MODE: 0..3. Frames are 8 bits.");
            return 2;
        }
        std::wstring command=argv[1];
        auto buses=DeviceInformation::FindAllAsync(SpiDevice::GetDeviceSelector()).get();
        hstring id;
        for(auto const& bus:buses){
            std::wstring path=bus.Id().c_str(),name=path.substr(path.find_last_of(L"\\")+1);
            if(command==L"list")wprintf(L"%s\n",name.c_str());
            if(argc>2&&name==argv[2])id=bus.Id();
        }
        if(command==L"list")return 0;
        if(argc<7||id.empty())throw std::runtime_error("Specify an available bus, chip select, clock, mode and data/count");
        unsigned cs=number(argv[3],1),hz=number(argv[4],4000000),mode=number(argv[5],3);
        if(hz<100000)throw std::runtime_error("Clock must be 100000..4000000 Hz");
        SpiConnectionSettings settings(cs);settings.ClockFrequency(hz);settings.Mode(static_cast<SpiMode>(mode));settings.DataBitLength(8);
        std::vector<uint8_t> tx,rx;
        if(command==L"read"){
            unsigned count=number(argv[6],65536);if(argc!=7||!count)throw std::runtime_error("Specify one nonzero count");rx.resize(count);
        }else if(command==L"write"||command==L"transfer"){
            for(int i=6;i<argc;++i)tx.push_back(static_cast<uint8_t>(number(argv[i],255)));
            if(command==L"transfer")rx.resize(tx.size());
        }else throw std::runtime_error("Unknown command");
        if(tx.size()+rx.size()>65536)throw std::runtime_error("Transfer exceeds 65536 bytes including both directions");
        auto d=SpiDevice::FromIdAsync(id,settings).get();if(!d)throw std::runtime_error("Cannot open target");
        if(command==L"write")d.Write(tx);
        else if(command==L"read")d.Read(rx);
        else d.TransferFullDuplex(tx,rx);
        d.Close();
        for(size_t i=0;i<rx.size();++i)printf("%02X%s",rx[i],(i+1)%16==0||i+1==rx.size()?"\n":" ");
        if(rx.empty())printf("Wrote %zu bytes\n",tx.size());
        return 0;
    }catch(hresult_error const& e){fwprintf(stderr,L"HRESULT %08X: %s\n",static_cast<unsigned>(e.code().value),e.message().c_str());return 1;}
    catch(std::exception const& e){fprintf(stderr,"%s\n",e.what());return 1;}
}
