// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.I2c.h>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>
#include <stdexcept>
using namespace winrt;
using namespace Windows::Devices::Enumeration;
using namespace Windows::Devices::I2c;
static unsigned number(const wchar_t* text,unsigned max) {
    wchar_t* end=nullptr;unsigned long n=wcstoul(text,&end,0);
    if(!*text||*end||*text==L'-'||n>max)throw std::runtime_error("Invalid number");
    return static_cast<unsigned>(n);
}
int wmain(int argc,wchar_t** argv) {
    try {
        init_apartment(apartment_type::multi_threaded);
        if(argc<2){
            puts("Pi5I2cTool list\nPi5I2cTool write BUS ADDRESS HZ BYTE...\nPi5I2cTool read BUS ADDRESS HZ COUNT\nPi5I2cTool write-read BUS ADDRESS HZ COUNT BYTE...\nNumbers accept decimal or 0x-prefixed hexadecimal. HZ: 100000 or 400000.");
            return 2;
        }
        std::wstring command=argv[1];
        auto buses=DeviceInformation::FindAllAsync(I2cDevice::GetDeviceSelector()).get();
        hstring id;
        for(auto const& bus:buses){
            std::wstring path=bus.Id().c_str(),name=path.substr(path.find_last_of(L"\\")+1);
            if(command==L"list")wprintf(L"%s\n",name.c_str());
            if(argc>2&&name==argv[2])id=bus.Id();
        }
        if(command==L"list")return 0;
        if(argc<6||id.empty())throw std::runtime_error("Specify an available bus, address, clock and data/count");
        unsigned address=number(argv[3],0x77),hz=number(argv[4],400000);
        if(address<8||(hz!=100000&&hz!=400000))throw std::runtime_error("Address must be 0x08..0x77; clock must be 100000 or 400000");
        I2cConnectionSettings settings(address);settings.BusSpeed(hz==100000?I2cBusSpeed::StandardMode:I2cBusSpeed::FastMode);
        std::vector<uint8_t> tx,rx;
        int first=5;
        if(command==L"read"||command==L"write-read"){
            unsigned count=number(argv[5],65536);if(!count)throw std::runtime_error("Count must be nonzero");rx.resize(count);first=6;
        }else if(command!=L"write")throw std::runtime_error("Unknown command");
        for(int i=first;i<argc;++i)tx.push_back(static_cast<uint8_t>(number(argv[i],255)));
        if(tx.size()+rx.size()>65536||(command!=L"read"&&tx.empty())||(command==L"read"&&!tx.empty()))throw std::runtime_error("Invalid transfer length");
        auto d=I2cDevice::FromIdAsync(id,settings).get();if(!d)throw std::runtime_error("Cannot open target");
        if(command==L"write")d.Write(tx);
        else if(command==L"read")d.Read(rx);
        else d.WriteRead(tx,rx);
        d.Close();
        for(size_t i=0;i<rx.size();++i)printf("%02X%s",rx[i],(i+1)%16==0||i+1==rx.size()?"\n":" ");
        if(rx.empty())printf("Wrote %zu bytes\n",tx.size());
        return 0;
    }catch(hresult_error const& e){
        unsigned code=static_cast<unsigned>(e.code().value);
        if(code==0x800701b1u||code==0x80070002u)fprintf(stderr,"No device acknowledged the I2C address (HRESULT %08X)\n",code);
        else fwprintf(stderr,L"HRESULT %08X: %s\n",code,e.message().c_str());
        return 1;
    }catch(std::exception const& e){fprintf(stderr,"%s\n",e.what());return 1;}
}
